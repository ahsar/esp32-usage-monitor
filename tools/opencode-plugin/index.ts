import { Plugin } from "@opencode/plugin"
import { spawn } from "node:child_process"
import { basename } from "node:path"
import { fileURLToPath } from "node:url"

/**
 * Pushes OpenCode's agent activity to the ESP32 usage monitor.
 *
 * The plugin is deliberately thin: it maps an event to a device query — the
 * state plus as much context as the event carries (repo / tool / note, see
 * `eventContext`) — and spawns `tools/monitor-send.sh`, which owns the whole
 * transport ladder —
 *
 *   1. the LAN over HTTP (mDNS `usage-monitor.local`, or any configured IP)
 *   2. …the further addresses configured here, in order
 *   3. BLE, one shot (`tools/ble-bridge.py --once`) — for a host the LAN
 *      cannot reach: a VLAN boundary, a blocked mDNS, or an AP that associates
 *      and then carries nothing
 *
 * The *order* is a client-side decision because the device cannot choose how
 * the host reaches it; keeping it in one hand-runnable script is what makes the
 * ladder testable without OpenCode. There is no daemon to start: a message that
 * falls through pays a cold BLE connect (~8-15 s) once, which is why the command
 * is given a generous budget and a failed invocation sits in a penalty box.
 *
 * Every request is fire-and-forget: a device that is off, asleep, or on another
 * network can never slow OpenCode down or surface an error.
 *
 * Load it from the global config:
 *
 *   {
 *     "plugins": [
 *       {
 *         "package": "/absolute/path/to/tools/opencode-plugin",
 *         "options": {
 *           "urls": ["http://usage-monitor.local", "http://192.168.20.31"],
 *           "key": "<status_key from data/config.json>"
 *         }
 *       }
 *     ]
 *   }
 */

type MonitorState = "running" | "waiting" | "done" | "failed"

/**
 * Map an OpenCode V2 event to a device state, or undefined to ignore it.
 *
 * These are the V2 event names as emitted by the server (verified against
 * opencode v2.0.x). V2 does not use V1's `session.idle` / `session.status` /
 * `permission.updated`.
 */
function mapEvent(type: string | undefined): MonitorState | undefined {
  switch (type) {
    case "session.execution.started":
      return "running"
    case "session.execution.succeeded":
      return "done"
    case "session.execution.interrupted":
      return "done"
    case "session.execution.failed":
      return "failed"
    case "permission.asked":
    case "question.asked":
      return "waiting"
    case "permission.replied":
    case "permission.rejected":
    case "question.replied":
    case "question.rejected":
      return "running"
    default:
      return undefined
  }
}

// The device's field caps (parse/agent_parse.h), as character counts. Clipping
// here rather than letting the device return 400: the context is advisory, and
// losing "the agent is in esp32-usage-monitor" because a permission pattern was
// one byte long would be the wrong trade. The device still validates, so a
// different client cannot push a 4 KB note at it.
const REPO_MAX = 27
const TOOL_MAX = 19
const NOTE_MAX = 51

/** One line, printable ASCII, within the device's cap. */
function clip(v: unknown, max: number): string | undefined {
  if (typeof v !== "string") return undefined
  // A permission pattern is often a whole command line (newlines included) and
  // the panel has one row; the device drops control bytes, this drops the
  // whitespace runs.
  const t = v
    .replace(/\s+/g, " ")
    // The generated Montserrat subset covers 0x20-0x7F plus °, ³, µ and •; a
    // CJK repo name would come out as tofu, so keep the line drawable.
    .replace(/[^\x20-\x7E]/g, "?")
    .trim()
  if (!t) return undefined
  // ".." rather than an ellipsis character: the LVGL font subset has no U+2026.
  return t.length > max ? t.slice(0, max - 2) + ".." : t
}

/**
 * Remember which directory a session is working in.
 *
 * `session.created` / `session.updated` carry `info.directory`; a permission
 * request only carries the session id. The map is bounded and only ever grows to
 * the number of live sessions, so there is nothing to evict on a timer — clearing
 * at 64 is enough to make a leak impossible.
 */
function rememberDirectory(event: any, dirs: Map<string, string>) {
  const p = event?.properties ?? {}
  const dir = p.info?.directory ?? p.directory ?? p.session?.directory
  const id = p.sessionID ?? p.info?.id
  if (typeof dir !== "string" || !dir || typeof id !== "string" || !id) return
  if (dirs.size > 64) dirs.clear()
  dirs.set(id, dir)
}

/**
 * What the session is working on, as far as this event says.
 *
 * `repo` is the session's project root, so the basename is the name a user
 * recognises; when no session directory has been seen (the event stream may not
 * include `session.created`), the service's own working directory is the next
 * best guess. The tool/permission text is read defensively from the event
 * payload: the field names below are the ones the plugin API documents
 * (`permission.asked` → `permission` + `patterns`, `question.asked` →
 * `questions[].header`), but a version that renames one must degrade to "no
 * tool"/"no note", never to a crash or a blank screen.
 */
function eventContext(
  event: any,
  dirs: Map<string, string>,
): { repo?: string; tool?: string; note?: string } {
  const props = event?.properties ?? {}

  let repo: string | undefined
  try {
    const dir = (props.sessionID && dirs.get(props.sessionID)) || process.cwd()
    repo = clip(basename(dir), REPO_MAX)
  } catch {
    repo = undefined
  }

  const tool = clip(props.permission ?? props.tool ?? props.toolName ?? props.name, TOOL_MAX)

  let note = clip(props.patterns?.[0], NOTE_MAX)
  if (!note) note = clip(props.questions?.[0]?.header, NOTE_MAX)
  if (!note) note = clip(props.questions?.[0]?.question, NOTE_MAX)

  return { repo, tool, note }
}

/**
 * The device query for one state. The `agent=` form carries the context and is
 * signed over the whole query string (tools/monitor-send.sh does that for
 * anything that is not a bare `state=…`), which is what lets the device draw
 * text a LAN peer cannot forge.
 *
 * done/failed deliberately carry no context, so the device's copy is cleared and
 * a later running state cannot show a stale repo from the previous run.
 */
function buildQuery(
  state: MonitorState,
  ctx: { repo?: string; tool?: string; note?: string },
): string {
  const parts = ["agent=1", `state=${state}`]
  for (const k of ["repo", "tool", "note"] as const) {
    const v = ctx[k]
    if (v) parts.push(`${k}=${encodeURIComponent(v)}`)
  }
  return parts.join("&")
}

/** The configured HTTP rungs: explicit `urls`, else `url`, else the env, else mDNS. */
function resolveUrls(options: any): string[] {
  const fromOptions: string[] | undefined =
    Array.isArray(options?.urls) && options.urls.length > 0
      ? options.urls
      : options?.url
        ? [options.url]
        : undefined

  const fromEnv: string[] | undefined = process.env.ESP32_MONITOR_URLS
    ? process.env.ESP32_MONITOR_URLS.split(",")
    : process.env.ESP32_MONITOR_URL
      ? [process.env.ESP32_MONITOR_URL]
      : undefined

  const raw = fromOptions ?? fromEnv ?? ["http://usage-monitor.local"]
  return raw.map((u) => String(u).trim().replace(/\/+$/, "")).filter((u) => u.length > 0)
}

/** The default is `tools/monitor-send.sh`, next to this plugin. */
const DEFAULT_SCRIPT = fileURLToPath(new URL("../monitor-send.sh", import.meta.url))

export default Plugin.define({
  id: "esp32-monitor",
  async setup(ctx: any) {
    const options = ctx?.options ?? {}
    // The script walks the whole ladder, so its budget has to cover the worst
    // case: the HTTP rungs (a couple of seconds each) *plus* a cold BLE connect,
    // which is 8-15 s on macOS and can be tried against a couple of targets.
    const sendTimeoutMs = Number(options.sendTimeoutMs ?? 60000)
    // How long a failed invocation is left alone. Expiry is also the recovery
    // path: the next message walks the ladder afresh, so a network that came
    // back is picked up without a restart.
    const penaltyMs = Number(options.penaltyMs ?? 30000)
    // Shared secret for the device's HMAC auth. Must match `status_key` in the
    // device's data/config.json. Omit it only when the device has no key; the
    // script falls back to reading that file itself.
    const key = String(options.key ?? process.env.ESP32_MONITOR_KEY ?? "")
    const ble = options.ble === false
    const debug = Boolean(process.env.ESP32_MONITOR_DEBUG)
    const script = String(options.sendScript ?? process.env.ESP32_MONITOR_SEND ?? DEFAULT_SCRIPT)

    const urls = resolveUrls(options)
    const env: Record<string, string> = {
      ...(process.env as Record<string, string>),
      ESP32_MONITOR_URLS: urls.join(","),
      ESP32_MONITOR_BLE: ble ? "0" : "1",
    }
    if (key) env.ESP32_MONITOR_KEY = key

    // Deduplicate: several events can report the same state (e.g. two
    // executions back to back), and only transitions should be sent. The key is
    // the whole query rather than the state, so a different tool inside one
    // running session still repaints — but a repeat of the same context does
    // not, which is what keeps this from forwarding every event.
    let lastQuery: string | undefined
    /** sessionID → project directory, learned from session events as they pass. */
    const sessionDirs = new Map<string, string>()
    /** Epoch ms until which the device is left alone after a failed send. */
    let deadUntil = 0

    const deliver = (query: string) =>
      new Promise<void>((resolve) => {
        if (Date.now() < deadUntil) return resolve()

        const child = spawn(script, [query], { env, stdio: ["ignore", "ignore", "pipe"] })

        let stderr = ""
        child.stderr?.on("data", (chunk) => {
          stderr += chunk
        })

        // Our own timer rather than spawn's `timeout`: the kill has to be
        // unconditional even on a runtime that ignores the option.
        const timer = setTimeout(() => child.kill("SIGKILL"), sendTimeoutMs)
        const done = (code: number | null) => {
          clearTimeout(timer)
          if (debug && stderr.trim()) console.error(stderr.trim())
          if (code === 0) {
            deadUntil = 0
            if (debug) console.error(`[esp32-monitor] ${query} → sent via ${script}`)
          } else if (code === 1) {
            // Nothing reached the device: out of the running for a while.
            deadUntil = Date.now() + penaltyMs
            if (debug) console.error(`[esp32-monitor] ${query} → unreachable (penalty ${penaltyMs}ms)`)
          } else {
            // A usage/spawn error (bad path, missing python, …) — do not
            // penalise, it will not fix itself, but do not hide it either.
            console.error(`[esp32-monitor] ${query} → send script exited ${code} (${script})`)
          }
          resolve()
        }

        child.on("error", (err) => {
          console.error(`[esp32-monitor] cannot run ${script}: ${String(err)}`)
          done(null)
        })
        child.on("close", done)
      })

    // Delivered one at a time, in order — so the device sees the states in the
    // order they happened (the chimes depend on it) and a slow BLE fallback
    // cannot be overlapped by the next event's HTTP probe.
    let queue: Promise<void> = Promise.resolve()
    const send = (query: string) => {
      queue = queue.then(() => deliver(query))
      return queue
    }

    if (debug) {
      console.error(
        `[esp32-monitor] ladder: ${urls.join(" → ")}${ble ? "" : " (ble off)"} via ${script} (auth ${key ? "on" : "off"})`,
      )
    }

    const controller = new AbortController()
    void (async () => {
      try {
        for await (const event of ctx.event.subscribe({ signal: controller.signal })) {
          // Learn the session's directory from whatever passes by (session
          // events carry it; the execution events may not), before the state
          // mapping filters the event out.
          rememberDirectory(event, sessionDirs)

          const state = mapEvent((event as { type?: string })?.type)
          if (!state) continue
          // Context on the two states that own the screen; done/failed send the
          // bare state so the device's copy is cleared (see buildQuery).
          const withContext = state === "running" || state === "waiting"
          const query = buildQuery(state, withContext ? eventContext(event, sessionDirs) : {})
          if (query === lastQuery) continue
          lastQuery = query
          void send(query)
        }
      } catch {
        // Stream aborted on unload.
      }
    })()

    return () => controller.abort()
  },
})

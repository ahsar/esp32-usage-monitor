import { Plugin } from "@opencode/plugin"
import { spawn } from "node:child_process"
import { fileURLToPath } from "node:url"

/**
 * Pushes OpenCode's agent activity to the ESP32 usage monitor.
 *
 * The plugin is deliberately thin: it maps an event to a state and spawns
 * `tools/monitor-send.sh`, which owns the whole transport ladder —
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
      return "waiting"
    case "permission.replied":
    case "permission.rejected":
      return "running"
    default:
      return undefined
  }
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
    // executions back to back), and only transitions should be sent.
    let last: MonitorState | undefined
    /** Epoch ms until which the device is left alone after a failed send. */
    let deadUntil = 0

    const deliver = (state: MonitorState) =>
      new Promise<void>((resolve) => {
        if (Date.now() < deadUntil) return resolve()

        const child = spawn(script, [state], { env, stdio: ["ignore", "ignore", "pipe"] })

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
            if (debug) console.error(`[esp32-monitor] ${state} → sent via ${script}`)
          } else if (code === 1) {
            // Nothing reached the device: out of the running for a while.
            deadUntil = Date.now() + penaltyMs
            if (debug) console.error(`[esp32-monitor] ${state} → unreachable (penalty ${penaltyMs}ms)`)
          } else {
            // A usage/spawn error (bad path, missing python, …) — do not
            // penalise, it will not fix itself, but do not hide it either.
            console.error(`[esp32-monitor] ${state} → send script exited ${code} (${script})`)
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
    const send = (state: MonitorState) => {
      queue = queue.then(() => deliver(state))
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
          const state = mapEvent((event as { type?: string })?.type)
          if (!state || state === last) continue
          last = state
          void send(state)
        }
      } catch {
        // Stream aborted on unload.
      }
    })()

    return () => controller.abort()
  },
})

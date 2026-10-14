import { Plugin } from "@opencode/plugin"
import { createHmac } from "node:crypto"

/**
 * Pushes OpenCode's agent activity to the ESP32 usage monitor's HTTP status
 * server (`http://usage-monitor.local/?state=running|waiting|done|failed`).
 *
 * The device is the server; this plugin is the client. Every request is
 * fire-and-forget with a short timeout, so a device that is off, asleep, or on
 * another network can never slow OpenCode down or surface an error.
 *
 * Load it from the global config:
 *
 *   {
 *     "plugins": [
 *       {
 *         "package": "/absolute/path/to/tools/opencode-plugin",
 *         "options": { "url": "http://usage-monitor.local" }
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

export default Plugin.define({
  id: "esp32-monitor",
  async setup(ctx: any) {
    const base = String(
      ctx?.options?.url ?? process.env.ESP32_MONITOR_URL ?? "http://usage-monitor.local",
    ).replace(/\/+$/, "")
    // Shared secret for the device's HMAC auth. Must match `status_key` in the
    // device's data/config.json. Omit it only when the device has no key.
    const key = String(ctx?.options?.key ?? process.env.ESP32_MONITOR_KEY ?? "")
    const timeoutMs = Number(ctx?.options?.timeoutMs ?? 1500)
    const debug = Boolean(process.env.ESP32_MONITOR_DEBUG)

    // Deduplicate: several events can report the same state (e.g. two
    // executions back to back), and only transitions should be sent.
    let last: MonitorState | undefined

    const headers = (state: MonitorState): Record<string, string> => {
      if (!key) return {}
      const ts = String(Math.floor(Date.now() / 1000))
      return {
        "X-Ts": ts,
        "X-Sig": createHmac("sha256", key).update(`${state}|${ts}`).digest("hex"),
      }
    }

    const send = async (state: MonitorState) => {
      const ac = new AbortController()
      const timer = setTimeout(() => ac.abort(), timeoutMs)
      try {
        const res = await fetch(`${base}/?state=${state}`, {
          headers: headers(state),
          signal: ac.signal,
        })
        if (debug) console.error(`[esp32-monitor] ${state} → ${res.status}`)
        // A 401 means the key is wrong/absent, not a transient failure: keep
        // `last` so we don't hammer the device on every event.
        if (res.status === 401 && debug) {
          console.error("[esp32-monitor] rejected — check `key` vs status_key")
        }
        if (res.status === 401) return
      } catch (err) {
        if (debug) console.error(`[esp32-monitor] ${state} failed: ${String(err)}`)
        // Unreachable device: forget the state so the next event retries it.
        if (last === state) last = undefined
      } finally {
        clearTimeout(timer)
      }
    }

    if (debug) console.error(`[esp32-monitor] monitoring ${base} (auth ${key ? "on" : "off"})`)

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

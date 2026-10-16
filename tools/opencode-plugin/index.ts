import { Plugin } from "@opencode/plugin"
import { createHmac } from "node:crypto"

/**
 * Pushes OpenCode's agent activity to the ESP32 usage monitor, over an ordered
 * list of transports.
 *
 * The device is the server; this plugin is the client. The same channel is
 * served over HTTP on the LAN (`src/status_server.cpp`) and over BLE
 * (`src/ble_status.cpp`), and the two do not always agree about being reachable:
 * a VLAN boundary, a blocked mDNS, or an AP that associates and then carries
 * nothing all break the LAN path without the device noticing. So the *order* is
 * a client-side decision — the device cannot choose how the host reaches it.
 *
 * The ladder is tried in order, and an endpoint that fails sits in a short
 * penalty box so a dead transport is not paid for on every message (it is
 * retried after `penaltyMs`, which is what brings the plugin back to the LAN
 * once the network recovers):
 *
 *   1. `urls[0]` (default http://usage-monitor.local)   — the LAN, preferred
 *   2. …any further `urls`                              — other addresses/VLANs
 *   3. the BLE bridge (default http://127.0.0.1:8098)   — see tools/ble-bridge.py
 *
 * If nothing answers the message is dropped: every request is fire-and-forget
 * with a short timeout, so a device that is off, asleep, or unreachable can
 * never slow OpenCode down or surface an error.
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

interface Endpoint {
  url: string
  /** How long to wait for a reply before treating the transport as down. */
  timeoutMs: number
  /** Short label for the debug log: "usage-monitor.local", "127.0.0.1:8098". */
  label: string
  /** Epoch ms until which this endpoint is skipped (0 = not penalised). */
  deadUntil: number
}

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

function labelOf(url: string): string {
  return url.replace(/^https?:\/\//, "").replace(/\/+$/, "") || url
}

/** The configured ladder: explicit `urls`, else `url`, else the env, else mDNS. */
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

export default Plugin.define({
  id: "esp32-monitor",
  async setup(ctx: any) {
    const options = ctx?.options ?? {}
    // The LAN is the transport we want to be using, so a probe of it must be
    // quick. The fallback needs a much bigger budget: the BLE bridge has to
    // establish a link before the device even hears the request, and measured on
    // macOS that is ~8-15 s cold (the first connect attempt often times out and
    // the retry succeeds) against ~0.5 s once the bridge holds the link. A cold
    // fallback that is abandoned mid-connect would never work at all.
    const timeoutMs = Number(options.timeoutMs ?? 1500)
    const fallbackTimeoutMs = Number(options.fallbackTimeoutMs ?? 30000)
    // How long a transport that failed is left alone. This is also the recovery
    // interval: once it expires the LAN is preferred again, so a network that
    // came back is picked up on the next message.
    const penaltyMs = Number(options.penaltyMs ?? 30000)
    // Shared secret for the device's HMAC auth. Must match `status_key` in the
    // device's data/config.json. Omit it only when the device has no key.
    const key = String(options.key ?? process.env.ESP32_MONITOR_KEY ?? "")
    // The bridge owns the BLE link and serves the same channel on loopback
    // (tools/ble-bridge.py). `ble: false` takes it out of the ladder.
    const ble = options.ble === false ? undefined : String(options.ble ?? process.env.ESP32_MONITOR_BLE ?? "http://127.0.0.1:8098")
    const debug = Boolean(process.env.ESP32_MONITOR_DEBUG)

    const urls = [...resolveUrls(options), ...(ble ? [ble.replace(/\/+$/, "")] : [])]
      // Deduplicate: listing the bridge twice would just double its timeout.
      .filter((u, i, all) => all.indexOf(u) === i)

    const endpoints: Endpoint[] = urls.map((url, i) => ({
      url,
      timeoutMs: i === 0 ? timeoutMs : fallbackTimeoutMs,
      label: labelOf(url),
      deadUntil: 0,
    }))

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

    const deliver = async (state: MonitorState) => {
      for (const ep of endpoints) {
        if (Date.now() < ep.deadUntil) continue

        const ac = new AbortController()
        const timer = setTimeout(() => ac.abort(), ep.timeoutMs)
        try {
          const res = await fetch(`${ep.url}/?state=${state}`, {
            headers: headers(state),
            signal: ac.signal,
          })
          if (debug) console.error(`[esp32-monitor] ${state} → ${res.status} via ${ep.label}`)

          // A 401 means the key is wrong/absent, not a transport problem, and
          // it will be just as wrong on the next transport: stop here rather
          // than burning the fallback's timeout, and keep `last` so we don't
          // hammer the device on every event.
          if (res.status === 401) {
            if (debug) console.error("[esp32-monitor] rejected — check `key` vs status_key")
            return
          }
          if (!res.ok) throw new Error(`HTTP ${res.status}`)

          ep.deadUntil = 0
          return
        } catch (err) {
          // Out of the running for a while; the next message tries the
          // transport after it, and this one comes back on its own later.
          ep.deadUntil = Date.now() + penaltyMs
          if (debug) console.error(`[esp32-monitor] ${state} failed via ${ep.label}: ${String(err)}`)
        } finally {
          clearTimeout(timer)
        }
      }

      if (debug) console.error(`[esp32-monitor] ${state}: no transport reached the device`)
      // Unreachable device: forget the state so the next event retries it.
      if (last === state) last = undefined
    }

    // Delivered one at a time, in order. Not for the device's sake — it takes one
    // request at a time anyway — but for the penalty box: a 1.5 s LAN probe easily
    // outlives the gap between two events, so overlapping probes would each pay the
    // same dead transport's timeout before either had recorded the penalty, and
    // each would spend a whole timeout on a transport the previous one had just
    // proved dead. Serialising costs nothing here (a message is only late, never
    // lost) and it also guarantees the device sees the states in the order they
    // happened, which is what the chimes depend on.
    let queue: Promise<void> = Promise.resolve()
    const send = (state: MonitorState) => {
      queue = queue.then(() => deliver(state))
      return queue
    }

    if (debug) {
      console.error(
        `[esp32-monitor] transports: ${endpoints.map((e) => `${e.label}(${e.timeoutMs}ms)`).join(" → ")} (auth ${key ? "on" : "off"})`,
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

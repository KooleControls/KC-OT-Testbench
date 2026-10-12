// Who talks to whom on the bench. A master (a thermostat) is paired with at most one
// slave (a boiler), and a slave with at most one master; either can be left open.
// The real devices are the ones on the module's two lines, the simulators are to
// live in the gateway.
//
// Only the masters' choices are stored: a slave's partner is whichever master chose
// it, so the two sides can never disagree.

export type Master = "thermostat" | "thermostatSim"
export type Slave = "boiler" | "boilerSim"
export type Endpoint = Master | Slave

export type Routing = Record<Master, Slave | null>

export const MASTERS: readonly Master[] = ["thermostat", "thermostatSim"]
export const SLAVES: readonly Slave[] = ["boiler", "boilerSim"]

/** The bench as it is wired today: the real thermostat straight to the real boiler. */
export const DEFAULT_ROUTING: Routing = { thermostat: "boiler", thermostatSim: null }

export function isMaster(endpoint: Endpoint): endpoint is Master {
  return (MASTERS as readonly Endpoint[]).includes(endpoint)
}

export function partnerOf(routing: Routing, endpoint: Endpoint): Endpoint | null {
  if (isMaster(endpoint)) return routing[endpoint]
  return MASTERS.find((m) => routing[m] === endpoint) ?? null
}

/** The routing with `endpoint` connected to `partner`, or left open for null. Whoever
 *  either of them was connected to before is left open. */
export function connect(routing: Routing, endpoint: Endpoint, partner: Endpoint | null): Routing {
  if (partner !== null && isMaster(partner) === isMaster(endpoint))
    throw new Error(`${endpoint} cannot connect to ${partner}: both are ${isMaster(partner) ? "masters" : "slaves"}`)

  const master = isMaster(endpoint) ? endpoint : (partner as Master | null)
  const slave = isMaster(endpoint) ? (partner as Slave | null) : endpoint

  const next = { ...routing }
  if (master === null) {
    // A slave left open: release it from whoever had it.
    for (const m of MASTERS) if (next[m] === slave) next[m] = null
    return next
  }
  for (const m of MASTERS) if (m !== master && slave !== null && next[m] === slave) next[m] = null
  next[master] = slave
  return next
}

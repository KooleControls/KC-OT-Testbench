// Who talks to whom on the bench: the names the testbench's `route get` / `route set`
// use. A master (a thermostat) is paired with at most one slave (a boiler) and the
// other way round; the testbench enforces that (RoutingManager), this only names them.

export type Master = "thermostat" | "thermostatSim"
export type Slave = "boiler" | "boilerSim"
export type Endpoint = Master | Slave

/** Each master's slave, null when it is open. */
export type Routing = Record<Master, Slave | null>

export const MASTERS: readonly Master[] = ["thermostat", "thermostatSim"]
export const SLAVES: readonly Slave[] = ["boiler", "boilerSim"]

export function isMaster(endpoint: Endpoint): endpoint is Master {
  return (MASTERS as readonly Endpoint[]).includes(endpoint)
}

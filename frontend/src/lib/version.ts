export function isPreRelease(version: string | undefined): boolean {
  if (!version) return false
  const parts = version.split(".")
  const patch = parts[2]
  return patch !== undefined && patch !== "0"
}

/// Returns true if `latest` is newer than `current` (semver comparison).
export function isNewerVersion(current: string, latest: string): boolean {
  const parse = (v: string) => v.split(".").map((n) => parseInt(n, 10) || 0)
  const c = parse(current)
  const l = parse(latest)
  for (let i = 0; i < 3; i++) {
    if ((l[i] ?? 0) > (c[i] ?? 0)) return true
    if ((l[i] ?? 0) < (c[i] ?? 0)) return false
  }
  return false
}

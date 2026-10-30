// Searching the Simulators page: which settings and rows match what was typed.
//
// Every term must match (AND), case-insensitively. A word matches anywhere in the
// item's text. A number has to be a whole word, so "25" finds data id 25 and not 125
// or a member id 0x25; "id 25", "id25" and "#25" are read the same way.

export interface Searchable {
  label: string
  description?: string
  /** The setting's key, e.g. "bsim.water". */
  key?: string
  unit?: string
  /** The OpenTherm data ids this item is carried in. */
  ids?: readonly number[]
  /** Other words someone might look for it by, e.g. its capability. */
  keywords?: string
}

export function searchTerms(query: string): string[] {
  return query
    .toLowerCase()
    .split(/\s+/)
    .flatMap((t) => {
      const id = /^(?:id|#)(\d+)$/.exec(t)
      return id ? ["id", id[1]] : [t]
    })
    .filter((t) => t !== "")
}

function haystack(item: Searchable): string {
  return [
    item.label,
    item.description,
    item.key,
    item.unit,
    item.keywords,
    ...(item.ids ?? []).map((id) => `data id ${id}`),
  ]
    .filter(Boolean)
    .join(" ")
    .toLowerCase()
}

/** True for no terms at all: an empty search shows everything. */
export function matchesSearch(item: Searchable, terms: readonly string[]): boolean {
  if (terms.length === 0) return true
  const text = haystack(item)
  const words = text.split(/[^a-z0-9.]+/)
  return terms.every((t) => (/^\d+$/.test(t) ? words.includes(t) : text.includes(t)))
}

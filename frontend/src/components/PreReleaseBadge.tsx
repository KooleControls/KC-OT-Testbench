import { isPreRelease } from "@/lib/version"

export function PreReleaseBadge({ version }: { version: string | undefined }) {
  if (!isPreRelease(version)) return null

  return (
    <span className="rounded-md bg-amber-500/15 px-1.5 py-0.5 text-[10px] font-semibold uppercase leading-none text-amber-500">
      Pre-release
    </span>
  )
}

import { useEffect, useState } from "react"
import { GITHUB_REPO } from "@/config"

export interface ReleaseInfo {
  version: string
  tag: string
  url: string
}

export function useLatestRelease() {
  const [release, setRelease] = useState<ReleaseInfo | null>(null)

  useEffect(() => {
    fetch(`https://api.github.com/repos/${GITHUB_REPO}/releases/latest`)
      .then((r) => (r.ok ? r.json() : null))
      .then((data) => {
        if (!data?.tag_name) return

        setRelease({
          version: data.tag_name.replace(/^v/, ""),
          tag: data.tag_name,
          url: data.html_url,
        })
      })
      .catch(() => {})
  }, [])

  return release
}

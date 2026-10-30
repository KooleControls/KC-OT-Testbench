import { useState, useEffect, useCallback } from "react"
import { PAGES, type Page } from "@/components/AppSidebar"

// The route lives in the HASH, not the path, and that is load-bearing rather
// than a style choice: this UI is served from two mount points the build cannot
// know — the device root (`/`) and the relay's `/devices/<id>/` — and a path
// route destroys the second one. Writing "/settings" dropped the device id, so
// F5 asked the relay for a URL it has no route for (404), and a reconnect
// resolved its socket against "/" instead of the device prefix.
//
// A hash never reaches the server, so the document URL stays the mount point
// forever: relative asset URLs (vite `base: "./"`) and resolveWsUrl's
// document-directory trick keep resolving, at any nesting depth, with no
// server-side SPA fallback involved. The prefix is never parsed here — the same
// reason backend.ts knows nothing about a device id.
function hashToPage(hash: string): Page {
  const segment = hash.replace(/^#\/?/, "").split("/")[0]?.toLowerCase()
  if (segment && PAGES.includes(segment as Page)) return segment as Page
  return "home"
}

function pageToHash(page: Page): string {
  return page === "home" ? "#/" : `#/${page}`
}

export function useRoute() {
  const [page, setPageState] = useState<Page>(() =>
    hashToPage(window.location.hash),
  )

  const navigate = useCallback((p: Page) => {
    // Assigning the hash pushes its own history entry, so back/forward keep
    // working without touching history directly.
    const hash = pageToHash(p)
    if (window.location.hash !== hash) window.location.hash = hash
    setPageState(p)
  }, [])

  useEffect(() => {
    const onHashChange = () => setPageState(hashToPage(window.location.hash))
    window.addEventListener("hashchange", onHashChange)
    return () => window.removeEventListener("hashchange", onHashChange)
  }, [])

  return { page, navigate }
}

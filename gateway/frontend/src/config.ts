// Fork configuration — the only frontend file with project-specific
// constants. Everything user-visible (device name, project name) comes
// from the device at runtime; these are the things the frontend needs
// before it can talk to a device.

/** Device hostname the dev server proxies to (`pnpm dev`). */
export const DEV_HOST = "kc-ot-testbench.local"

/** GitHub repo checked for new releases (update dot in the sidebar). */
export const GITHUB_REPO = "KooleControls/KC-OT-Testbench"

/** Static brand shown on the login page (pre-auth). Post-auth the real device
 *  name comes from `getInfo`. Forks customize this. */
export const PRODUCT_NAME = "KC-OT-Testbench"

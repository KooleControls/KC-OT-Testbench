import { useEffect } from "react"
import {
  ActivityIcon,
  CableIcon,
  CpuIcon,
  NetworkIcon,
  TerminalIcon,
  SquareTerminalIcon,
  SettingsIcon,
  DownloadIcon,
} from "lucide-react"
import {
  Sidebar,
  SidebarContent,
  SidebarFooter,
  SidebarGroup,
  SidebarGroupContent,
  SidebarHeader,
  SidebarMenu,
  SidebarMenuButton,
  SidebarMenuItem,
} from "@/components/ui/sidebar"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { useDeviceInfo } from "@/hooks/use-device-info"
import { useLatestRelease } from "@/hooks/use-latest-release"
import { isNewerVersion } from "@/lib/version"
import { PreReleaseBadge } from "@/components/PreReleaseBadge"
import { DeviceInfoDialog } from "@/components/DeviceInfoDialog"
import { ThemeToggle } from "@/components/ThemeToggle"

// The nav, and the first entry is the product: the OpenTherm frames. "home" is a fixed
// id rather than the feature's name, so a bookmark to "/" lands on the product's own
// screen. The RS485 bus is the second protocol's frames, so it stands next to them. The Strux template's LED demo has no page here; its commands are still in
// Commands.
//
// There is no "Device" entry: a chip name and a heap figure are reference material
// you go looking for, not a destination, so they live behind the footer instead of
// taking a place in the navigation beside the product. See
// docs/reasoning/2026-09-09-22h00.
//
// Two of them carry a `hint`, and only those two: Console and Commands both look
// like terminals and are not the same thing at all — one is what the device
// printed, the other is what you asked it. Naming the difference in the nav is
// cheaper than finding it out by clicking.
const navItems = [
  { title: "Frames", icon: ActivityIcon, page: "home" as const },
  { title: "RS485", icon: NetworkIcon, page: "rs485" as const },
  { title: "Connections", icon: CableIcon, page: "connections" as const },
  { title: "Simulators", icon: CpuIcon, page: "simulators" as const },
  {
    title: "Console",
    hint: "Device logs (stdout)",
    icon: TerminalIcon,
    page: "console" as const,
  },
  {
    title: "Commands",
    hint: "Run device commands",
    icon: SquareTerminalIcon,
    page: "commands" as const,
  },
  { title: "Settings", icon: SettingsIcon, page: "settings" as const },
  { title: "Firmware", icon: DownloadIcon, page: "firmware" as const },
]

export type Page = (typeof navItems)[number]["page"]

// The routable pages, derived from the nav rather than written out again.
// use-route needs the same list to decide whether a hash segment is a real
// page, and a second hand-maintained copy could not be type-checked: a
// `Page[]` missing an entry is a shorter valid array, so a new page appeared
// in the nav and silently routed back to home when clicked.
export const PAGES: readonly Page[] = navItems.map((i) => i.page)

interface AppSidebarProps {
  currentPage: Page
  onNavigate: (page: Page) => void
}

const statusColor = {
  connected: "bg-emerald-500",
  connecting: "bg-amber-500 animate-pulse",
  disconnected: "bg-red-500",
} as const

const statusLabel = {
  connected: "Online",
  connecting: "Connecting",
  disconnected: "Offline",
} as const

export function AppSidebar({ currentPage, onNavigate }: AppSidebarProps) {
  const connection = useConnectionStatus()
  const info = useDeviceInfo()
  const release = useLatestRelease()
  const updateAvailable = info && release && isNewerVersion(info.firmware, release.version)

  // Browser tab title follows the device name (login page covers pre-auth).
  useEffect(() => {
    if (info?.name) document.title = info.name
  }, [info?.name])

  return (
    <Sidebar>
      <SidebarHeader className="px-4 py-3">
        <div className="flex items-center gap-2">
          <span className="text-sm font-semibold">{info?.name ?? "…"}</span>
          <PreReleaseBadge version={info?.firmware} />
          <ThemeToggle />
        </div>
      </SidebarHeader>
      <SidebarContent>
        <SidebarGroup>
          <SidebarGroupContent>
            <SidebarMenu>
              {navItems.map((item) => (
                <SidebarMenuItem key={item.page}>
                  <SidebarMenuButton
                    isActive={currentPage === item.page}
                    onClick={() => onNavigate(item.page)}
                    className={"hint" in item ? "h-auto py-1.5" : undefined}
                  >
                    <item.icon />
                    <span className="flex min-w-0 flex-col">
                      <span>{item.title}</span>
                      {"hint" in item && (
                        <span className="truncate text-xs text-muted-foreground">
                          {item.hint}
                        </span>
                      )}
                    </span>
                    {item.page === "firmware" && updateAvailable && (
                      <span className="ml-auto h-2 w-2 rounded-full bg-emerald-500" />
                    )}
                  </SidebarMenuButton>
                </SidebarMenuItem>
              ))}
            </SidebarMenu>
          </SidebarGroupContent>
        </SidebarGroup>
      </SidebarContent>
      {/* The footer is the way in to the device's details. It already shows the
          version and the link state, so it is where somebody looks when they want to
          know more about either. A button, not a div with an onClick: keyboard focus
          and Enter come for free, and a dialog reached only by mouse is a dialog some
          people cannot reach. */}
      <SidebarFooter className="p-3">
        <DeviceInfoDialog>
          <button
            type="button"
            aria-label="Device info"
            className="w-full cursor-pointer rounded-lg border bg-card p-3 text-left text-xs transition-colors hover:bg-muted focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
          >
            {info && (
              <div className="mb-1.5 flex items-center justify-between">
                <span className="text-muted-foreground">Version</span>
                <div className="flex items-center gap-1.5">
                  <PreReleaseBadge version={info.firmware} />
                  <span className="font-mono">{info.firmware}</span>
                </div>
              </div>
            )}
            <div className="flex items-center justify-between">
              <span className="text-muted-foreground">Status</span>
              <div className="flex items-center gap-1.5">
                <span className={`h-2 w-2 rounded-full ${statusColor[connection]}`} />
                <span>{statusLabel[connection]}</span>
              </div>
            </div>
          </button>
        </DeviceInfoDialog>
      </SidebarFooter>
    </Sidebar>
  )
}

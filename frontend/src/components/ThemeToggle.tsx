import { useTheme } from "next-themes"
import { MonitorIcon, MoonIcon, SunIcon } from "lucide-react"
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip"

// Three states, one button. "system" is a real choice and not the absence of one --
// it is what a device UI opened from a phone at night should do by default -- so it
// stays in the cycle instead of being a hidden initial value you can never get back
// to once you have touched the toggle.
const order = ["system", "light", "dark"] as const
type Theme = (typeof order)[number]

const icon = { system: MonitorIcon, light: SunIcon, dark: MoonIcon }
const label = { system: "System theme", light: "Light theme", dark: "Dark theme" }

export function ThemeToggle() {
  const { theme, setTheme } = useTheme()

  // useTheme returns undefined until the provider has read localStorage, which is
  // one render. Treating that as "system" keeps the icon from flipping.
  const current = (order.includes(theme as Theme) ? theme : "system") as Theme
  const Icon = icon[current]

  return (
    <Tooltip>
      <TooltipTrigger
        type="button"
        aria-label={label[current]}
        onClick={() => setTheme(order[(order.indexOf(current) + 1) % order.length])}
        className="ml-auto rounded-md p-1.5 text-muted-foreground transition-colors hover:bg-muted hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
      >
        <Icon className="size-4" />
      </TooltipTrigger>
      <TooltipContent>{label[current]}</TooltipContent>
    </Tooltip>
  )
}

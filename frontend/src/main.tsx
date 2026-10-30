import { StrictMode } from "react"
import { createRoot } from "react-dom/client"
import { ThemeProvider } from "next-themes"
import { TooltipProvider } from "@/components/ui/tooltip"
import { Toaster } from "@/components/ui/sonner"

import "./index.css"
import App from "./App.tsx"

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    {/* attribute="class" is what `@custom-variant dark` in index.css keys on, so this
        provider is the only thing that ever puts the dark tokens in play. The choice
        lives in the BROWSER's localStorage, not in device settings: it is a property
        of who is looking, and two people on one device should not fight over it. */}
    <ThemeProvider attribute="class" defaultTheme="system" enableSystem disableTransitionOnChange>
      <TooltipProvider>
        <App />
        <Toaster richColors />
      </TooltipProvider>
    </ThemeProvider>
  </StrictMode>
)

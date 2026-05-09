"""
SimWorkbench InitGui.py — FreeCAD Simulation Workbench registration.

This file is loaded by FreeCAD's GUI at startup when the addon is installed.
It registers:
  - The SimWorkbench workbench with FreeCAD's workbench selector
  - All toolbar/menu commands
  - A delayed startup hook that loads the dock panels

Installation:
  Copy addons/SimWorkbench/ to %APPDATA%\\FreeCAD\\v1-2\\Mod\\SimWorkbench\\
  Restart FreeCAD and switch to "Simulation Workbench" in the workbench dropdown.

Architecture:
  InitGui.py (this file)
    └── sim_workbench.py   SimWorkbenchCoordinator (lifecycle)
          ├── transport.py   GazeboTransport (polls Gazebo state via bridge module)
          ├── state_bridge.py  StateBridge (Gazebo poses → FreeCAD placements)
          └── panels/        Qt dock widgets
                ├── sim_controls.py     Play / Pause / Step / Reset / RTF
                ├── scenario_picker.py  Robot / World / Scenario selection
                ├── sensor_plots.py     Joint states and RTF chart
                ├── run_library.py      Browse sim_runs/
                └── mcp_log.py          MCP activity log
"""

import sys
import os

# Make the addon directory importable as a package root
_addon_dir = os.path.dirname(os.path.abspath(__file__))
if _addon_dir not in sys.path:
    sys.path.insert(0, _addon_dir)

# Also ensure the repo root is on the path so bridge/ is importable
_repo_root = os.path.abspath(os.path.join(_addon_dir, "..", ".."))
if _repo_root not in sys.path:
    sys.path.insert(0, _repo_root)


class SimWorkbench(Workbench):
    """
    FreeCAD Simulation Workbench.

    Provides a human cockpit for running and watching headless Gazebo simulations:
      - Sim Controls:      start / pause / step / reset / show RTF
      - Scenario Picker:   select robot, world, initial pose, scenario file
      - Live 3D View:      Gazebo poses reflected into FreeCAD model placements
      - Sensor Plots:      joint states, IMU, contacts in a live chart panel
      - Run Library:       browse previous sim_runs/
      - MCP Activity Log:  audit trail of LLM agent tool calls
    """

    MenuText = "Simulation Workbench"
    ToolTip  = "Run and watch headless Gazebo simulations from FreeCAD"

    # Icon: use a simple gear icon (bundled with FreeCAD) as a fallback
    Icon = os.path.join(_addon_dir, "icons", "SimWorkbench.svg")

    def Initialize(self):
        """Called once when the workbench is first activated."""
        from commands import sim_commands
        self.appendToolbar("Simulation", sim_commands.TOOLBAR_COMMANDS)
        self.appendMenu("Simulation", sim_commands.MENU_COMMANDS)

    def Activated(self):
        """Called every time the workbench is switched to."""
        try:
            from sim_workbench import get_coordinator
            coord = get_coordinator()
            coord.show_panels()
        except Exception as exc:
            FreeCAD.Console.PrintWarning(f"[SimWorkbench] Failed to show panels: {exc}\n")

    def Deactivated(self):
        """Called when the user switches away from this workbench."""
        try:
            from sim_workbench import get_coordinator
            coord = get_coordinator()
            coord.hide_panels()
        except Exception:
            pass

    def ContextMenu(self, recipient):
        pass

    def GetClassName(self):
        return "Gui::PythonWorkbench"


Gui.addWorkbench(SimWorkbench())

FreeCAD.Console.PrintMessage("[SimWorkbench] Simulation Workbench loaded.\n")

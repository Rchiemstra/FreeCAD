"""
sim_commands.py — FreeCAD command definitions for the Simulation Workbench.

Each command maps to a toolbar / menu action.  Commands delegate to the
SimWorkbenchCoordinator so no simulation logic lives here.
"""
from __future__ import annotations

import os

import FreeCAD
import FreeCADGui


_ICON = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "icons",
    "SimWorkbench.svg",
)
_READ_WRITE_ICON = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "icons",
    "ReadModeEyeRed.svg",
)


def _coord():
    """Lazily fetch the singleton coordinator."""
    from sim_workbench import get_coordinator
    return get_coordinator()


# ---------------------------------------------------------------------------
# Command: Start Simulation
# ---------------------------------------------------------------------------

class StartSimulationCommand:
    """Export, spawn model, and start the Gazebo simulation clock."""

    def GetResources(self):
        return {
            "Pixmap":  _ICON,
            "MenuText": "Start Simulation",
            "ToolTip":  "Export the robot, spawn it in Gazebo, and start the clock.",
        }

    def IsActive(self):
        return True

    def Activated(self):
        msg = _coord().start_sim()
        FreeCAD.Console.PrintMessage(f"[SimWorkbench] {msg}\n")


FreeCADGui.addCommand("SimWB_StartSimulation", StartSimulationCommand())


# ---------------------------------------------------------------------------
# Command: Pause Simulation
# ---------------------------------------------------------------------------

class PauseSimulationCommand:
    def GetResources(self):
        return {
            "Pixmap":  _ICON,
            "MenuText": "Pause Simulation",
            "ToolTip":  "Pause the Gazebo physics clock.",
        }

    def IsActive(self):
        return True

    def Activated(self):
        msg = _coord().pause_sim()
        FreeCAD.Console.PrintMessage(f"[SimWorkbench] {msg}\n")


FreeCADGui.addCommand("SimWB_PauseSimulation", PauseSimulationCommand())


# ---------------------------------------------------------------------------
# Command: Resume Simulation
# ---------------------------------------------------------------------------

class ResumeSimulationCommand:
    def GetResources(self):
        return {
            "Pixmap":  _ICON,
            "MenuText": "Resume Simulation",
            "ToolTip":  "Resume the Gazebo physics clock.",
        }

    def IsActive(self):
        return True

    def Activated(self):
        msg = _coord().resume_sim()
        FreeCAD.Console.PrintMessage(f"[SimWorkbench] {msg}\n")


FreeCADGui.addCommand("SimWB_ResumeSimulation", ResumeSimulationCommand())


# ---------------------------------------------------------------------------
# Command: Step Simulation
# ---------------------------------------------------------------------------

class StepSimulationCommand:
    def GetResources(self):
        return {
            "Pixmap":  _ICON,
            "MenuText": "Step Simulation",
            "ToolTip":  "Advance the simulation by one physics step.",
        }

    def IsActive(self):
        return True

    def Activated(self):
        msg = _coord().step_sim(1)
        FreeCAD.Console.PrintMessage(f"[SimWorkbench] {msg}\n")


FreeCADGui.addCommand("SimWB_StepSimulation", StepSimulationCommand())


# ---------------------------------------------------------------------------
# Command: Reset Simulation
# ---------------------------------------------------------------------------

class ResetSimulationCommand:
    def GetResources(self):
        return {
            "Pixmap":  _ICON,
            "MenuText": "Reset Simulation",
            "ToolTip":  "Reset the Gazebo world to its initial state.",
        }

    def IsActive(self):
        return True

    def Activated(self):
        msg = _coord().reset_sim()
        FreeCAD.Console.PrintMessage(f"[SimWorkbench] {msg}\n")


FreeCADGui.addCommand("SimWB_ResetSimulation", ResetSimulationCommand())


# ---------------------------------------------------------------------------
# Command: Toggle Document Read Mode
# ---------------------------------------------------------------------------

class ToggleReadModeCommand:
    def GetResources(self):
        return {
            "Pixmap": _READ_WRITE_ICON,
            "MenuText": "Toggle Document Read Mode",
            "ToolTip": (
                "Watch the active saved FreeCAD document and reload it when an "
                "external agent regenerates the file."
            ),
        }

    def IsActive(self):
        doc = getattr(FreeCAD, "ActiveDocument", None)
        return bool(doc is not None and getattr(doc, "FileName", ""))

    def Activated(self):
        from read_mode import toggle_read_mode
        msg = toggle_read_mode()
        FreeCAD.Console.PrintMessage(f"[SimWorkbench] {msg}\n")


FreeCADGui.addCommand("SimWB_ToggleReadMode", ToggleReadModeCommand())


# ---------------------------------------------------------------------------
# Command lists referenced from InitGui.py
# ---------------------------------------------------------------------------

TOOLBAR_COMMANDS = [
    "SimWB_StartSimulation",
    "SimWB_PauseSimulation",
    "SimWB_ResumeSimulation",
    "SimWB_StepSimulation",
    "SimWB_ResetSimulation",
    "SimWB_ToggleReadMode",
]

MENU_COMMANDS = TOOLBAR_COMMANDS

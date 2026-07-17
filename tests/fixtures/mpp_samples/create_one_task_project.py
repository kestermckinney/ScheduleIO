# Copyright (C) 2026 Paul McKinney
#
# Creates a brand-new Microsoft Project file with a single task and saves it.
#
# Follows the same COM-automation approach as newmsproject_plugin.py:
#   - win32com.client.Dispatch("MSProject.Application")
#   - drive the running Project instance through its object model
#   - FileSaveAs to write the .mpp to disk
#
# Usage:
#   python create_one_task_project.py [output.mpp] ["Task name"] [duration]
#
# Defaults are used for any argument that is omitted.

import os
import sys
import time
import platform

if platform.system() != 'Windows':
    sys.exit("This script requires Microsoft Project on Windows.")

import win32com.client

# --- MS Project COM enum constants ------------------------------------------
# PjFileFormat
PJ_MPP = 0            # native .mpp
# PjSaveType (used by Quit / FileCloseEx)
PJ_DO_NOT_SAVE = 0    # discard without prompting


def create_one_task_project(output_path, task_name, duration, start_date):
    """Create a new project containing a single task, then save it as .mpp."""

    output_path = os.path.abspath(output_path)

    # Connect to Microsoft Project (starts it if it is not already running).
    project = win32com.client.Dispatch("MSProject.Application")
    project.Visible = 1

    # Start a fresh, empty project. FileNew returns before the new document is
    # fully initialised, so wait for ActiveProject to become available.
    project.FileNew()

    deadline = time.time() + 30
    while project.ActiveProject is None and time.time() < deadline:
        time.sleep(0.25)
    if project.ActiveProject is None:
        raise RuntimeError("Timed out waiting for the new project to initialise.")

    proj = project.ActiveProject

    # Add the single task. Tasks.Add(Name) appends it to the schedule.
    task = proj.Tasks.Add(task_name)

    # New tasks default to manually scheduled; set an explicit start + duration
    # so the one-task schedule has real dates. Duration accepts a string such
    # as "5d" (5 days) which Project's scheduling engine parses.
    task.Start = start_date
    task.Duration = duration

    # Overwrite silently if a file with this name already exists.
    if os.path.exists(output_path):
        os.remove(output_path)

    # Save as a native .mpp file.
    project.FileSaveAs(output_path, PJ_MPP)

    # Close the document without prompting to save, then quit Project.
    project.FileCloseEx(PJ_DO_NOT_SAVE)
    project.Quit(PJ_DO_NOT_SAVE)

    return output_path


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else "one_task_project.mpp"
    name = sys.argv[2] if len(sys.argv) > 2 else "Design Phase"
    dur = sys.argv[3] if len(sys.argv) > 3 else "5d"
    start = "07/20/2026 8:00 AM"

    saved = create_one_task_project(out, name, dur, start)
    print(f"Created Microsoft Project file: {saved}")

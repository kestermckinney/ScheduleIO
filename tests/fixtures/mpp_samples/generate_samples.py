# Copyright (C) 2026 Paul McKinney
#
# Generates a diverse set of Microsoft Project sample files (.mpp) and their
# corresponding MSPDI XML exports (.xml) for reverse-engineering / testing the
# MPP binary format. Special emphasis on FONT FORMATTING options.
#
# Automation approach mirrors newmsproject_plugin.py: drive a live
# MSProject.Application COM instance through its object model.
#
# ---------------------------------------------------------------------------
# CRITICAL SAFETY RULE (per project requirement):
#   Never let Python garbage-collection tear down the MSProject.Application COM
#   object while a document is still open -- Project would pop a modal
#   "Do you want to save?" dialog and hang. Every code path MUST explicitly:
#     1. close each open document with pjDoNotSave (or FileSave it first), and
#     2. Quit(pjDoNotSave)
#   BEFORE the last Python reference is released. The MSProjectApp context
#   manager below guarantees this even when an exception is raised.
# ---------------------------------------------------------------------------
#
# NOTE ON XML + FONTS: Project's native XML (MSPDI) is a *data* schema. It does
# NOT store cell-level font formatting or text styles (colors, bold, fonts).
# That presentation data lives only inside the .mpp binary. For the font
# samples, manifest.json is the authoritative record of what was applied where.

import os
import gc
import sys
import time
import json
import platform

if platform.system() != 'Windows':
    sys.exit("This script requires Microsoft Project on Windows.")

import win32com.client
from win32com.client import gencache

OUT_ROOT = os.path.dirname(os.path.abspath(__file__))


def rgb(r, g, b):
    """Windows COLORREF integer used by MS Project font/color APIs."""
    return r + (g * 256) + (b * 65536)


class MSProjectApp:
    """Context manager that guarantees Project is shut down cleanly.

    On exit it force-closes every open document without saving and quits the
    application, so a garbage-collected COM object can never leave a modal
    "save changes?" dialog behind.
    """

    def __init__(self, visible=True):
        self.visible = visible
        self.app = None
        self.C = None  # enum constants

    def __enter__(self):
        # EnsureDispatch gives early binding + named enum constants.
        self.app = gencache.EnsureDispatch("MSProject.Application")
        self.C = win32com.client.constants
        try:
            self.app.Visible = 1 if self.visible else 0
        except Exception:
            pass
        return self

    def __exit__(self, exc_type, exc, tb):
        self.shutdown()
        return False  # never suppress exceptions

    def shutdown(self):
        if self.app is None:
            return
        app, self.app = self.app, None
        # 1) Close every open document, discarding changes, so nothing is dirty.
        try:
            guard = 0
            while app.Projects.Count > 0 and guard < 100:
                try:
                    app.FileCloseEx(self.C.pjDoNotSave)  # 0 = do not save
                except Exception:
                    break
                guard += 1
        except Exception:
            pass
        # 2) Quit, again discarding anything still open.
        try:
            app.Quit(self.C.pjDoNotSave)
        except Exception:
            pass
        app = None
        gc.collect()

    # -- helpers -------------------------------------------------------------

    def new_project(self):
        """Create a new blank project and wait until it is ready to edit."""
        self.app.FileNew()
        deadline = time.time() + 30
        while self.app.ActiveProject is None and time.time() < deadline:
            time.sleep(0.25)
        if self.app.ActiveProject is None:
            raise RuntimeError("Timed out waiting for new project to initialise.")
        time.sleep(0.3)
        return self.app.ActiveProject

    def make_children(self, rows, level=2):
        """Make the given task rows children at `level` by setting OutlineLevel
        directly. This is far more reliable than OutlineIndent(), which is
        relative to the current structure and easily nests siblings into each
        other."""
        proj = self.app.ActiveProject
        for r in rows:
            proj.Tasks(r).OutlineLevel = level
            time.sleep(0.05)

    def close_active(self):
        """Close the active document without saving (leaves app running)."""
        try:
            self.app.FileCloseEx(self.C.pjDoNotSave)
        except Exception:
            pass

    def save_outputs(self, base_name):
        """Save the active project as both <base>.mpp and <base>.xml.

        Returns (mpp_path, xml_path). The .mpp is written last so the on-disk
        binary is authoritative.
        """
        folder = os.path.join(OUT_ROOT, base_name)
        os.makedirs(folder, exist_ok=True)
        mpp = os.path.join(folder, base_name + ".mpp")
        xml = os.path.join(folder, base_name + ".xml")
        for p in (mpp, xml):
            if os.path.exists(p):
                os.remove(p)
        # Pin the project Title before either export. Without this the XML
        # (saved while the document is still the unsaved "ProjectN") carries
        # Title/UID-0-name "ProjectN" while the .mpp (saved after, under its
        # real file name) stores the base name -- the pair then disagrees on
        # Title and on the project summary task's Name for no real reason.
        self.app.ActiveProject.Title = base_name
        # XML (MSPDI) export first...
        self.app.FileSaveAs(xml, self.C.pjMPP, False, False, False, False,
                            "", "", "", "MSProject.XML")
        # ...then the native binary.
        self.app.FileSaveAs(mpp, self.C.pjMPP)
        return mpp, xml

    def set_cell_font(self, row, column, name=None, size=None, bold=False,
                      italic=False, underline=False, color=None,
                      cell_color=None, pattern=0, strike=False):
        """Apply direct cell-level font formatting to one task field.

        Uses Application.Font32Ex after selecting the target cell.
        Signature: Font32Ex(Name, Size, Bold, Italic, Underline, Color,
                            Reset, CellColor, Pattern, Strikethrough)

        RowRelative=False is essential: SelectTaskField's Row is RELATIVE to
        the current selection by default, so successive calls drift down the
        sheet cumulatively (this is exactly what put the v1 sample's formats
        on rows 2/4/7/11/16 instead of 1/2/3/4/5 -- visible in the preview
        screenshot and confirmed in the binary's TABLE_FONT_STYLES records).
        """
        self.app.SelectTaskField(row, column, False)
        self.app.Font32Ex(
            name if name is not None else "",
            size if size is not None else 0,
            bool(bold), bool(italic), bool(underline),
            color if color is not None else 0,
            False,
            cell_color if cell_color is not None else 0,
            pattern,
            bool(strike),
        )


# ===========================================================================
# Sample builders. Each takes an MSProjectApp, builds one project, saves it,
# and returns a short status string. Each is wrapped by run() so one failing
# feature never aborts the whole batch.
# ===========================================================================

def build_01_font_cell_formatting(mp):
    """Matrix of direct cell-level font formatting (Font32Ex)."""
    proj = mp.new_project()

    rows = [
        # name, font, size, bold, italic, underline, strike, color, cellcolor
        ("Default Calibri 11pt (baseline)", "Calibri", 11, 0, 0, 0, 0, rgb(0, 0, 0), None),
        ("Arial 18pt Bold Red", "Arial", 18, 1, 0, 0, 0, rgb(255, 0, 0), None),
        ("Times New Roman 10pt Italic", "Times New Roman", 10, 0, 1, 0, 0, rgb(0, 0, 0), None),
        ("Calibri 14pt Underline+Strike Blue", "Calibri", 14, 0, 0, 1, 1, rgb(0, 0, 255), None),
        ("Verdana 12pt Bold+Italic Green", "Verdana", 12, 1, 1, 0, 0, rgb(0, 176, 80), None),
        ("Georgia 16pt Underline Purple", "Georgia", 16, 0, 0, 1, 0, rgb(128, 0, 128), None),
        ("Courier New 11pt Orange", "Courier New", 11, 0, 0, 0, 0, rgb(255, 165, 0), None),
        ("Comic Sans MS 20pt Bold", "Comic Sans MS", 20, 1, 0, 0, 0, rgb(0, 0, 0), None),
        ("Tahoma 9pt Italic+Underline Gray", "Tahoma", 9, 0, 1, 1, 0, rgb(128, 128, 128), None),
        ("Trebuchet 24pt Bold+Ital+Und+Strike Red", "Trebuchet MS", 24, 1, 1, 1, 1, rgb(255, 0, 0), None),
        ("Segoe UI 8pt (smallest)", "Segoe UI", 8, 0, 0, 0, 0, rgb(0, 0, 0), None),
        ("Arial 28pt Bold Navy (largest)", "Arial", 28, 1, 0, 0, 0, rgb(0, 0, 128), None),
        ("Wingdings 14pt (symbol font)", "Wingdings", 14, 0, 0, 0, 0, rgb(0, 0, 0), None),
        ("Arial 12pt Magenta RGB(255,0,255)", "Arial", 12, 0, 0, 0, 0, rgb(255, 0, 255), None),
        ("Arial 12pt Teal RGB(0,128,128)", "Arial", 12, 0, 0, 0, 0, rgb(0, 128, 128), None),
        ("Arial 12pt Bold Olive RGB(128,128,0)", "Arial", 12, 1, 0, 0, 0, rgb(128, 128, 0), None),
        ("Arial 6pt (extreme tiny)", "Arial", 6, 0, 0, 0, 0, rgb(0, 0, 0), None),
        ("Arial 36pt (extreme huge)", "Arial", 36, 1, 0, 0, 0, rgb(200, 0, 0), None),
    ]

    for name, *_ in rows:
        proj.Tasks.Add(name)
    time.sleep(0.4)

    manifest = {
        "description": "Direct cell-level font (character) formatting applied "
                       "via Application.Font32Ex: font family, size, bold, "
                       "italic, underline, strikethrough, and text colour. NOT "
                       "captured in the .xml export (MPP-binary-only "
                       "presentation data). Cell BACKGROUND colour is "
                       "demonstrated separately in 10_cell_background.",
        "colorFormat": "COLORREF integer = R + G*256 + B*65536",
        "rows": [],
    }

    for i, (name, font, size, b, it, un, st, col, cc) in enumerate(rows, start=1):
        pattern = 2 if cc is not None else 0
        mp.set_cell_font(i, "Name", name=font, size=size, bold=b, italic=it,
                         underline=un, color=col, cell_color=cc,
                         pattern=pattern, strike=st)
        manifest["rows"].append({
            "row": i, "column": "Name", "taskName": name, "font": font,
            "size": size, "bold": bool(b), "italic": bool(it),
            "underline": bool(un), "strikethrough": bool(st),
            "colorCOLORREF": col, "cellBackgroundCOLORREF": cc,
            "pattern": pattern,
        })

    mpp, xml = mp.save_outputs("01_font_cell_formatting")
    with open(os.path.join(os.path.dirname(mpp), "manifest.json"), "w",
              encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    mp.close_active()
    return f"{len(rows)} tasks, cell fonts applied"


def build_02_text_styles(mp):
    """Category-level text styling via TextStylesEx (applies to whole classes
    of rows: summaries, milestones, critical tasks, etc.)."""
    proj = mp.new_project()
    C = mp.C

    # A small schedule with a summary, subtasks (one critical path), a milestone.
    proj.Tasks.Add("Phase 1 (summary)")
    t2 = proj.Tasks.Add("Design")
    t3 = proj.Tasks.Add("Build")
    t4 = proj.Tasks.Add("Test")
    proj.Tasks.Add("Phase 1 Complete (milestone)")
    time.sleep(0.3)

    for t in (t2, t3, t4):
        try:
            t.Manual = False
        except Exception:
            pass
        t.Duration = "3d"
    # make 2..4 children of task 1 to create the summary
    try:
        mp.make_children([2, 3, 4])
    except Exception:
        pass
    # make the last task a milestone, and mark task 3 (for the "marked" style)
    try:
        proj.Tasks(5).Milestone = True
        proj.Tasks(5).Duration = "0d"
        proj.Tasks(3).Marked = True
    except Exception:
        pass
    time.sleep(0.3)

    applied = []
    # TextStylesEx(Item, Font, Size, Bold, Italic, Underline, Color). Two big
    # differences from cell-level Font32Ex:
    #   * The optional CellColor/Pattern args must be OMITTED -- passing 0 for
    #     them is rejected as "argument value is not valid".
    #   * Color is a PjColor PALETTE INDEX (0-15), NOT a COLORREF. 255 = the
    #     "automatic" colour. (Cell fonts take a full COLORREF; styles do not.)
    # PjItem:  pjAll=0 pjNoncritical=1 pjCritical=2 pjMilestone=3 pjSummary=4
    #          pjProjectSummary=5 pjMarked=6 pjTaskRowColumnTitles=8
    # PjColor: Black=0 Red=1 Yellow=2 Lime=3 Aqua=4 Blue=5 Fuchsia=6 White=7
    #          Maroon=8 Green=9 Olive=10 Navy=11 Teal=12 Purple=13 Silver=14 Gray=15
    styles = [
        ("pjSummary", 4, "Cambria", 12, 1, 0, 0, 11),   # Navy
        ("pjMilestone", 3, "Arial", 11, 1, 1, 0, 13),   # Purple
        ("pjCritical", 2, "Arial", 11, 0, 0, 0, 1),     # Red
        ("pjMarked", 6, "Arial", 11, 0, 1, 0, 9),       # Green
        ("pjTaskRowColumnTitles", 8, "Segoe UI", 11, 1, 0, 0, 5),  # Blue
    ]
    for item_name, item_default, font, size, b, it, un, color_index in styles:
        try:
            item = getattr(C, item_name, item_default)
            mp.app.TextStylesEx(item, font, size, bool(b), bool(it), bool(un),
                                color_index)
            applied.append(item_name)
        except Exception as e:
            applied.append(f"{item_name}:FAIL({type(e).__name__})")

    mp.save_outputs("02_text_styles")
    mp.close_active()
    return "styles: " + ", ".join(applied)


def build_03_hierarchy_dependencies(mp):
    """Outline hierarchy, all four link types with lag, and constraints."""
    proj = mp.new_project()
    C = mp.C

    names = ["Project Kickoff", "Requirements", "Architecture",
             "Development", "Module A", "Module B", "Integration",
             "QA & Testing", "Go Live"]
    tasks = [proj.Tasks.Add(n) for n in names]
    time.sleep(0.3)
    for t in tasks:
        try:
            t.Manual = False
        except Exception:
            pass
    tasks[0].Milestone = True
    tasks[0].Duration = "0d"
    for t in tasks[1:]:
        t.Duration = "5d"
    tasks[-1].Milestone = True
    tasks[-1].Duration = "0d"

    # Make Module A / Module B (tasks 5,6) children of Development (task 4).
    try:
        mp.make_children([5, 6])
    except Exception:
        pass
    time.sleep(0.3)

    # Dependencies are created through the writable Predecessors text field,
    # which accepts the full "<id><type><+/-lag>" syntax. (The object model has
    # no TaskDependencies collection on the document.) Link types: FS, SS, FF,
    # SF; lag/lead expressed as "+1d" / "-1d".
    link_report = []
    links = [
        (2, "1"),        # FS  (default)
        (3, "2FS+1d"),   # FS with +1 day lag
        (5, "3"),        # FS  Module A after Architecture
        (6, "5SS"),      # SS  Module B starts with Module A
        (7, "6FF"),      # FF  Integration finishes with Module B
        (8, "7SF"),      # SF  Start-to-Finish demo
        (9, "8FS-1d"),   # FS with -1 day lead to the Go-Live milestone
    ]
    for tid, pred in links:
        try:
            proj.Tasks(tid).Predecessors = pred
            link_report.append(f"{tid}<-{pred}")
        except Exception as e:
            link_report.append(f"{tid}:FAIL({type(e).__name__})")

    # Constraints on a couple of tasks.
    try:
        tasks[2].ConstraintType = C.pjSNET
        tasks[2].ConstraintDate = "08/03/2026 8:00 AM"
    except Exception:
        pass
    try:
        tasks[7].ConstraintType = C.pjMSO
        tasks[7].ConstraintDate = "09/15/2026 8:00 AM"
    except Exception:
        pass

    mp.save_outputs("03_hierarchy_dependencies")
    mp.close_active()
    return "links: " + ",".join(link_report)


def build_04_resources_assignments(mp):
    """Work, material and cost resources with assignments."""
    proj = mp.new_project()
    C = mp.C

    # Resources with distinct types.
    r_alice = proj.Resources.Add("Alice Engineer")
    r_bob = proj.Resources.Add("Bob Contractor")
    r_steel = proj.Resources.Add("Steel")
    r_travel = proj.Resources.Add("Travel")
    time.sleep(0.3)

    notes = []
    try:
        r_alice.Type = C.pjResourceTypeWork
        r_alice.StandardRate = "75/h"
        r_bob.Type = C.pjResourceTypeWork
        r_bob.StandardRate = "120/h"
        r_bob.MaxUnits = 0.5   # 50% availability -> overallocation demo
        r_steel.Type = C.pjResourceTypeMaterial
        r_steel.MaterialLabel = "tons"
        r_steel.StandardRate = "500"
        r_travel.Type = C.pjResourceTypeCost
        notes.append("types+rates set")
    except Exception as e:
        notes.append(f"rate/type FAIL({type(e).__name__})")

    t1 = proj.Tasks.Add("Fabrication")
    t2 = proj.Tasks.Add("Assembly")
    t3 = proj.Tasks.Add("Site Visit")
    time.sleep(0.3)
    for t in (t1, t2, t3):
        try:
            t.Manual = False
        except Exception:
            pass
        t.Duration = "4d"

    # Assign via task.Assignments.Add(TaskUID, ResourceUID). The ResourceNames
    # text field does NOT split on ";" under COM (it creates a single literal
    # resource), so add each assignment explicitly and set its Units/Cost.
    def assign(task, resource, units=None, cost=None):
        a = task.Assignments.Add(task.UniqueID, resource.UniqueID)
        if units is not None:
            a.Units = units
        if cost is not None:
            a.Cost = cost
        return a

    try:
        assign(t1, r_alice, units=1.0)      # full-time work
        assign(t1, r_steel, units=10)       # 10 tons of material
        assign(t2, r_alice, units=0.5)      # half-time
        assign(t2, r_bob, units=1.0)        # Bob @100% but MaxUnits 50% -> overallocated
        assign(t3, r_bob, units=1.0)
        assign(t3, r_travel, cost=500)      # cost resource: fixed $500
        notes.append("assignments set")
    except Exception as e:
        notes.append(f"assign FAIL({type(e).__name__})")

    mp.save_outputs("04_resources_assignments")
    mp.close_active()
    return "; ".join(notes)


def build_05_calendar_durations(mp):
    """Duration-unit variety, elapsed durations, and a calendar exception."""
    proj = mp.new_project()

    specs = [
        ("30 minutes", "30m"),
        ("4 hours", "4h"),
        ("1 day", "1d"),
        ("3 days", "3d"),
        ("2 weeks", "2w"),
        ("5 elapsed days (spans weekend)", "5ed"),
        ("Milestone (zero duration)", "0d"),
    ]
    tasks = []
    for name, _ in specs:
        tasks.append(proj.Tasks.Add(name))
    time.sleep(0.3)
    for (name, dur), t in zip(specs, tasks):
        try:
            t.Manual = False
        except Exception:
            pass
        t.Duration = dur
    try:
        tasks[-1].Milestone = True
    except Exception:
        pass

    note = "durations set"
    # Best-effort calendar exception: make a company holiday non-working.
    try:
        cal = proj.Calendar
        cal.Exceptions.Add(1, "07/03/2026", "07/03/2026")  # type 1 = daily
        note += "; calendar exception added"
    except Exception as e:
        note += f"; calendar exception FAIL({type(e).__name__})"

    mp.save_outputs("05_calendar_durations")
    mp.close_active()
    return note


def build_06_unicode_notes(mp):
    """Unicode / special-character task names, notes, hyperlink, deadline."""
    proj = mp.new_project()

    names = [
        "ASCII baseline task",
        "Accented: café résumé naïve Zürich",
        "CJK: プロジェクト 项目 프로젝트",
        "Cyrillic/Greek: Проект Δοκιμή",
        "Emoji: milestone \U0001F3AF ship \U0001F680 done ✅",
        "Symbols: <tag> & \"quotes\" 'apos' — 100% → ∞",
        "Very long name " + ("x" * 180),
    ]
    tasks = [proj.Tasks.Add(n) for n in names]
    time.sleep(0.3)
    for t in tasks:
        try:
            t.Manual = False
        except Exception:
            pass
        t.Duration = "2d"

    note = "unicode names set"
    try:
        tasks[0].Notes = ("Multi-line note.\r\nLine 2 with unicode ☃ and "
                          "tab\tafter. Special <>&\" chars included.")
        note += "; notes set"
    except Exception as e:
        note += f"; notes FAIL({type(e).__name__})"
    try:
        tasks[1].HyperlinkAddress = "https://example.com/schedule"
        tasks[1].Hyperlink = "Project site"
        note += "; hyperlink set"
    except Exception as e:
        note += f"; hyperlink FAIL({type(e).__name__})"
    try:
        tasks[4].Deadline = "12/31/2026 5:00 PM"
        note += "; deadline set"
    except Exception as e:
        note += f"; deadline FAIL({type(e).__name__})"

    mp.save_outputs("06_unicode_notes")
    mp.close_active()
    return note


def build_07_baseline_progress(mp):
    """Baseline snapshot plus % complete and actuals."""
    proj = mp.new_project()

    names = ["Planning", "Execution", "Closeout"]
    tasks = [proj.Tasks.Add(n) for n in names]
    time.sleep(0.3)
    for t in tasks:
        try:
            t.Manual = False
        except Exception:
            pass
        t.Duration = "5d"
    # chain them
    try:
        tasks[1].Predecessors = str(tasks[0].ID)
        tasks[2].Predecessors = str(tasks[1].ID)
    except Exception:
        pass
    time.sleep(0.3)

    # NOTE: app.BaselineSave() with no args pops the interactive "Set Baseline"
    # dialog and blocks automation. Instead write the baseline fields directly
    # on each task -- this stores baseline 0 with no UI.
    note = ""
    try:
        for t in tasks:
            t.BaselineStart = t.Start
            t.BaselineFinish = t.Finish
            t.BaselineDuration = t.Duration
        note += "baseline fields set"
    except Exception as e:
        note += f"baseline FAIL({type(e).__name__})"
    try:
        tasks[0].PercentComplete = 100
        tasks[1].PercentComplete = 40
        note += "; % complete set"
    except Exception as e:
        note += f"; %complete FAIL({type(e).__name__})"

    mp.save_outputs("07_baseline_progress")
    mp.close_active()
    return note


def build_08_edge_empty(mp):
    """Edge case: a completely empty project (no tasks)."""
    mp.new_project()
    mp.save_outputs("08_edge_empty")
    mp.close_active()
    return "empty project (0 tasks)"


def build_10_cell_background(mp):
    """Cell BACKGROUND colour (fill) via Font32Ex CellColor + Pattern.

    Pattern semantics, verified against MS Project's own rendering of this
    sample (the earlier "pattern=1 renders no fill" claim was an artifact of
    the SelectTaskField relative-row drift putting fills on other rows):
    0 = transparent, 1 = SOLID, 2 = light dotted, 4 = heavy dotted -- the
    stored value equals the COM argument and matches MPXJ's BackgroundPattern
    enum. CellColor is a COLORREF like the text colour. Like all font
    formatting, this is stored only in the .mpp binary (never in the XML
    export)."""
    proj = mp.new_project()

    # Short single-line names: multi-line (wrapped) task names make Project
    # draw the cell fill misaligned from the row, so keep these terse.
    rows = [
        # name, text_color, cell_color, pattern
        ("Yellow p1 solid", rgb(0, 0, 0), rgb(255, 255, 0), 1),
        ("Lime p1 solid", rgb(0, 0, 0), rgb(0, 255, 0), 1),
        ("Red p1 solid white text", rgb(255, 255, 255), rgb(255, 0, 0), 1),
        ("Navy p1 solid white text", rgb(255, 255, 255), rgb(0, 0, 128), 1),
        ("Cyan p2 light-dotted", rgb(0, 0, 0), rgb(0, 255, 255), 2),
        ("Silver p2 light-dotted", rgb(0, 0, 0), rgb(192, 192, 192), 2),
        ("Yellow p4 heavy-dotted", rgb(0, 0, 0), rgb(255, 255, 0), 4),
        ("Yellow p0 transparent", rgb(0, 0, 0), rgb(255, 255, 0), 0),
    ]
    for name, *_ in rows:
        proj.Tasks.Add(name)
    time.sleep(0.4)

    for i, (name, txt, cc, pat) in enumerate(rows, start=1):
        mp.set_cell_font(i, "Name", name="Arial", size=12, color=txt,
                         cell_color=cc, pattern=pat)

    mpp, _ = mp.save_outputs("10_cell_background")

    manifest = {
        "description": "Cell BACKGROUND colour (fill) applied via "
                       "Application.Font32Ex CellColor+Pattern. Stored "
                       "pattern equals the COM argument: 0=transparent, "
                       "1=solid, 2=light dotted, 4=heavy dotted (MPXJ "
                       "BackgroundPattern). Binary-only presentation data "
                       "(never in the .xml export).",
        "colorFormat": "COLORREF integer = R + G*256 + B*65536",
        "rows": [],
    }
    for i, (name, txt, cc, pat) in enumerate(rows, start=1):
        manifest["rows"].append({
            "row": i,
            "column": "Name",
            "taskName": name,
            "font": "Arial",
            "size": 12,
            "bold": False,
            "italic": False,
            "underline": False,
            "strikethrough": False,
            "colorCOLORREF": txt,
            "cellBackgroundCOLORREF": cc,
            "pattern": pat,
        })
    with open(os.path.join(os.path.dirname(mpp), "manifest.json"), "w",
              encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    mp.close_active()
    return f"{len(rows)} tasks, cell backgrounds applied"


def build_09_edge_milestone_only(mp):
    """Edge case: a single milestone."""
    proj = mp.new_project()
    t = proj.Tasks.Add("Contract Signed")
    time.sleep(0.2)
    try:
        t.Manual = False
    except Exception:
        pass
    t.Duration = "0d"
    t.Milestone = True
    mp.save_outputs("09_edge_milestone_only")
    mp.close_active()
    return "single milestone"


SAMPLES = [
    ("01_font_cell_formatting", build_01_font_cell_formatting),
    ("02_text_styles", build_02_text_styles),
    ("03_hierarchy_dependencies", build_03_hierarchy_dependencies),
    ("04_resources_assignments", build_04_resources_assignments),
    ("05_calendar_durations", build_05_calendar_durations),
    ("06_unicode_notes", build_06_unicode_notes),
    ("07_baseline_progress", build_07_baseline_progress),
    ("08_edge_empty", build_08_edge_empty),
    ("09_edge_milestone_only", build_09_edge_milestone_only),
    # 10 was originally excluded because the fill "rendered on the wrong rows"
    # -- that was the SelectTaskField relative-Row drift (see set_cell_font),
    # not a Project rendering quirk. Re-enabled now that selection is absolute.
    ("10_cell_background", build_10_cell_background),
]


def main():
    only = set(sys.argv[1:])  # optional: run only named samples
    results = []
    with MSProjectApp(visible=True) as mp:
        for name, fn in SAMPLES:
            if only and name not in only:
                continue
            try:
                status = fn(mp)
                results.append((name, "OK", status))
                print(f"[OK]   {name}: {status}")
            except Exception as e:
                # Make sure the failed doc doesn't linger and dirty the app.
                mp.close_active()
                results.append((name, "ERROR", f"{type(e).__name__}: {e}"))
                print(f"[FAIL] {name}: {type(e).__name__}: {e}")
    print("\nSummary:")
    for name, state, status in results:
        print(f"  {state:5} {name}  {status}")


if __name__ == '__main__':
    main()

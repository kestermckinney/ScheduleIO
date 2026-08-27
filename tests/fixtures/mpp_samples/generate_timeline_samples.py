# Copyright (C) 2026 Paul McKinney
#
# Generates a matrix of Microsoft Project sample files (.mpp + MSPDI .xml +
# manifest.json) that isolate one aspect of the **Timeline view** each, for
# reverse-engineering the `<TLViewData>` XML document that MS Project stores in
# the .mpp (CV_iew var-record type 47 == Props9 item key 574619695).
#
# Companion to generate_samples.py; same MSProjectApp shutdown-safety contract,
# same save_outputs() convention. MSPDI XML cannot represent any of the Timeline
# state, so manifest.json is the authoritative oracle -- exactly like the font
# samples.
#
# The Timeline object model is thinly documented. This script:
#   1. writes comprobe.json -- a reflection dump of every Application member whose
#      name hints at Timeline/bar/callout/detail plus the result of trying each
#      candidate call once -- so a failed batch still teaches us the real API;
#   2. drives what does work (at minimum Application.AddToTimeline for
#      membership) and records, per fixture, exactly which COM calls succeeded in
#      manifest.json["comLog"].
#
# Run on dpr2hw3 via the passwordless /ru+/it scheduled task (see
# ScheduleVault/tools + memory reference-remote-msproject-testing). Optional
# argv = subset of fixture names to (re)build.

import os
import gc
import sys
import time
import json
import platform

if platform.system() != "Windows":
    sys.exit("This script requires Microsoft Project on Windows.")

import win32com.client

OUT_ROOT = os.path.dirname(os.path.abspath(__file__))

# The handful of Project enum values we actually use. Hardcoded because we bind
# late (see dispatch_msproject) so win32com.client.constants is not populated.
PJ_DO_NOT_SAVE = 2
PJ_MPP = 0


def dispatch_msproject():
    """Late-bound Dispatch only. gencache.EnsureDispatch hangs / raises
    'has no attribute CLSIDToClassMap' on dpr2hw3 (py3.14 gen_py cache), so do
    not touch the makepy cache at all."""
    return win32com.client.Dispatch("MSProject.Application")


class MSProjectApp:
    """Context manager guaranteeing Project shuts down without a modal prompt.
    Copied from generate_samples.py -- keep behaviour identical."""

    def __init__(self, visible=True):
        self.visible = visible
        self.app = None
        self.C = None

    def __enter__(self):
        self.app = dispatch_msproject()
        try:
            self.C = win32com.client.constants
        except Exception:
            self.C = None
        try:
            self.app.Visible = 1 if self.visible else 0
        except Exception:
            pass
        return self

    def __exit__(self, exc_type, exc, tb):
        self.shutdown()
        return False

    def shutdown(self):
        if self.app is None:
            return
        app, self.app = self.app, None
        try:
            guard = 0
            while app.Projects.Count > 0 and guard < 100:
                try:
                    app.FileCloseEx(PJ_DO_NOT_SAVE)
                except Exception:
                    break
                guard += 1
        except Exception:
            pass
        try:
            app.Quit(PJ_DO_NOT_SAVE)
        except Exception:
            pass
        app = None
        gc.collect()

    # -- helpers (mirrors generate_samples.py) -----------------------------

    def new_project(self):
        self.app.FileNew()
        deadline = time.time() + 30
        while self.app.ActiveProject is None and time.time() < deadline:
            time.sleep(0.25)
        if self.app.ActiveProject is None:
            raise RuntimeError("Timed out waiting for new project to initialise.")
        time.sleep(0.3)
        return self.app.ActiveProject

    def make_children(self, rows, level=2):
        proj = self.app.ActiveProject
        for r in rows:
            proj.Tasks(r).OutlineLevel = level
            time.sleep(0.05)

    def close_active(self):
        try:
            self.app.FileCloseEx(PJ_DO_NOT_SAVE)
        except Exception:
            pass

    def save_outputs(self, base_name):
        folder = os.path.join(OUT_ROOT, base_name)
        os.makedirs(folder, exist_ok=True)
        mpp = os.path.join(folder, base_name + ".mpp")
        xml = os.path.join(folder, base_name + ".xml")
        for p in (mpp, xml):
            if os.path.exists(p):
                os.remove(p)
        self.app.ActiveProject.Title = base_name
        self.app.FileSaveAs(xml, PJ_MPP, False, False, False, False,
                            "", "", "", "MSProject.XML")
        self.app.FileSaveAs(mpp, PJ_MPP)
        return mpp, xml

    # -- timeline helpers -------------------------------------------------

    def view_apply(self, name):
        try:
            self.app.ViewApply(name)
            time.sleep(0.2)
            return True
        except Exception:
            try:
                self.app.ViewApplyEx(name)
                time.sleep(0.2)
                return True
            except Exception:
                return False

    def select_task(self, task_id):
        """Select one task row in the active (Gantt) grid, absolute row."""
        try:
            self.app.SelectTaskField(task_id, "Name", False)
            return True
        except Exception:
            try:
                self.app.SelectRow(task_id, False)
                return True
            except Exception:
                return False

    def try_call(self, log, label, fn):
        """Run fn(); append (label, 'ok'|'ERR:<type>') to log; return bool."""
        try:
            fn()
            time.sleep(0.1)
            log.append(f"{label}: ok")
            return True
        except Exception as e:
            log.append(f"{label}: ERR {type(e).__name__}: {e}")
            return False

    # -- real Timeline API (found via typelib walk; see memory
    #    reference-mpp-timeline-tlviewdata). All on MSProject.Application.
    #    NEVER call these with a missing arg where a dialog would otherwise
    #    show -- always pass ShowDialog / explicit values.

    def add_bar(self, log, task_ids, bar_index=0):
        """Add each task id to the timeline as a BAR on bar_index."""
        added = []
        for tid in task_ids:
            if self.try_call(log, f"TaskOnTimelineEx(id={tid},bar={bar_index})",
                             lambda t=tid: self.app.TaskOnTimelineEx(
                                 t, False, "", False, bar_index)):
                added.append(tid)
        return added

    # NOTE: TimelineInsertTask / TimelineGotoSelectedTask are deliberately NOT
    # used -- they open an interactive picker and hang detached automation.
    # Callout / text-only display style is therefore out of scope for the COM
    # matrix; RE it later by hand-editing <TLViewData> or via UI Automation.

    def _tl(self):
        self.view_apply("Timeline")

    def show_hide(self, log, item, show):
        self._tl()
        return self.try_call(log, f"TimelineShowHide(item={item},show={show})",
                             lambda: self.app.TimelineShowHide(item, show))

    def timeline_format(self, log, num_lines=1, minimized=True):
        self._tl()
        return self.try_call(
            log, f"TimelineFormat(lines={num_lines},min={minimized})",
            lambda: self.app.TimelineFormat(num_lines, minimized))

    def bar_label(self, log, label, bar_index):
        self._tl()
        return self.try_call(log, f"TimelineBarSetLabel('{label}',{bar_index})",
                             lambda: self.app.TimelineBarSetLabel(label, bar_index))

    def bar_range(self, log, start, finish, bar_index):
        self._tl()
        return self.try_call(
            log, f"TimelineBarDateRange(1,{start},{finish},{bar_index})",
            lambda: self.app.TimelineBarDateRange(True, start, finish, bar_index))

    def insert_bar(self, log, bar_index=1):
        self._tl()
        return self.try_call(log, f"InsertTimelineBar({bar_index})",
                             lambda: self.app.InsertTimelineBar(bar_index))


# ---------------------------------------------------------------------------
# COM reflection probe -- written once per batch as comprobe.json.
# ---------------------------------------------------------------------------

TIMELINE_HINTS = ("timeline", "callout", "panzoom", "pan_zoom", "detailbar",
                  "detailed", "overlap", "todayline", "tlview")


# Candidate Application member names for Timeline operations (late binding gives
# no usable dir(), so we probe names one by one).
NAME_CANDIDATES = [
    "AddToTimeline", "RemoveFromTimeline", "AddTaskToTimeline",
    "DisplayAsBar", "DisplayAsCallout", "DisplayAsTextLine", "DisplayAsText",
    "TimelineDisplayAsBar", "TimelineDisplayAsCallout", "TimelineDisplayAsText",
    "InsertTimelineBar", "TimelineBarInsert", "TimelineBarAdd", "AddTimelineBar",
    "TimelineBarDate", "TimelineBarDateRange", "SetTimelineBarDates",
    "TimelineDetailedView", "DisplayDetails", "TimelineText", "TextStylesEx",
    "PanZoom", "TimelinePanZoom", "OptionsViewEx", "ViewApply", "ViewApplyEx",
    "ViewEditSingle", "TimelineCopy", "GanttWizard",
]


def _name_exists(app, name):
    """True if the late-bound COM object resolves this member name."""
    try:
        app._oleobj_.GetIDsOfNames(0, name)
        return True
    except Exception:
        return False


# invkind: 1=method 2=propget 4=propput 8=propputref
def _func_sig(info, fd):
    try:
        names = info.GetNames(fd.memid)          # [funcName, arg1, arg2, ...]
        fname = names[0]
        argnames = names[1:]
    except Exception:
        return None
    params = []
    for k, a in enumerate(getattr(fd, "args", ()) or ()):
        vt = a[0][0] if isinstance(a[0], tuple) else a[0]
        params.append({"name": argnames[k] if k < len(argnames) else f"p{k}",
                       "vt": vt})
    return {"name": fname, "invkind": fd.invkind,
            "nparams": len(params), "params": params}


def _walk_typelib(disp, sig_ifaces=(), enum_prefixes=()):
    """Return a dict:
      members:  {interfaceName: [memberName, ...]}  (everything)
      sigs:     {iface: [funcSig, ...]}             (for sig_ifaces)
      enums:    {enumTypeName: {member: intValue}}  (for typeinfos whose name
                                                     starts with an enum_prefix)
    Late-bound safe (no gen_py)."""
    import pythoncom  # noqa: F401
    out = {"members": {}, "sigs": {}, "enums": {}}
    try:
        ti = disp._oleobj_.GetTypeInfo(0)
        tlb, _idx = ti.GetContainingTypeLib()
    except Exception as e:
        out["_error"] = f"{type(e).__name__}: {e}"
        return out
    for i in range(tlb.GetTypeInfoCount()):
        try:
            iname = tlb.GetDocumentation(i)[0]
            info = tlb.GetTypeInfo(i)
            attr = info.GetTypeAttr()
            members = set()
            for j in range(attr.cFuncs):
                fd = info.GetFuncDesc(j)
                try:
                    members.add(info.GetNames(fd.memid)[0])
                except Exception:
                    pass
                if iname in sig_ifaces:
                    sig = _func_sig(info, fd)
                    if sig:
                        out["sigs"].setdefault(iname, []).append(sig)
            is_enum = any(iname.startswith(p) for p in enum_prefixes)
            for j in range(attr.cVars):
                vd = info.GetVarDesc(j)
                try:
                    vn = info.GetNames(vd.memid)[0]
                    members.add(vn)
                    if is_enum:
                        out["enums"].setdefault(iname, {})[vn] = vd.value
                except Exception:
                    pass
            if members:
                out["members"][iname] = sorted(members)
        except Exception:
            continue
    return out


def _write_probe(report):
    with open(os.path.join(OUT_ROOT, "comprobe.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)


def com_probe(mp):
    """Pure reflection only. NEVER invoke a no-arg MS Project 'command' method
    here -- several (OptionsViewEx, TimelineBarDateRange, ...) pop a modal dialog
    and hang the whole detached automation. Enumeration is done via
    GetIDsOfNames + a type-library walk, neither of which invokes anything."""
    app = mp.app
    report = {"binding": "late (Dispatch)", "namesResolved": [],
              "namesMissing": [], "timelineMembers": {}}

    for n in NAME_CANDIDATES:
        (report["namesResolved"] if _name_exists(app, n)
         else report["namesMissing"]).append(n)
    _write_probe(report)          # checkpoint before the (slower) typelib walk
    print("com_probe: name resolution done", flush=True)

    lib = _walk_typelib(
        app,
        sig_ifaces=("_Global", "_MSProject", "Application", "_Application"),
        enum_prefixes=("PjTimeline", "PjViewType", "PjViewScreen", "PjColor",
                       "PjItem", "PjFont"))
    members = lib.get("members", {})
    report["typelibInterfaces"] = sorted(members.keys())
    report["timelineSigs"] = [
        s for s in lib.get("sigs", {}).get("_Global", [])
        if "timeline" in s["name"].lower() or "panzoom" in s["name"].lower()
        or "detail" in s["name"].lower()]
    report["enums"] = lib.get("enums", {})
    hint = ("timeline", "callout", "panzoom", "overlap", "tlview", "tline")
    for iname, mm in members.items():
        hits = [m for m in mm if any(h in m.lower() for h in hint)]
        if hits:
            report["timelineMembers"][iname] = hits
    for key in ("_Global", "Task"):
        if key in members:
            report[f"members::{key}"] = members[key]
    _write_probe(report)
    print("com_probe: typelib walk done", flush=True)
    return "comprobe.json written"


# ---------------------------------------------------------------------------
# Base schedule shared by every fixture.
# ---------------------------------------------------------------------------

def build_base(mp):
    """8 auto-scheduled tasks: 1 summary (children 2,3), 2 milestones, a chain.
    Returns the ActiveProject. Task IDs are 1..9 (1 = summary)."""
    proj = mp.new_project()
    names = ["Phase A (summary)", "Design", "Build",
             "Gate 1 (milestone)", "Integrate", "Verify",
             "Ship (milestone)", "Support"]
    for n in names:
        proj.Tasks.Add(n)
    time.sleep(0.3)
    for i in range(1, 9):
        try:
            proj.Tasks(i).Manual = False
        except Exception:
            pass
    try:
        mp.make_children([2, 3])           # Design, Build under Phase A
    except Exception:
        pass
    for i in (2, 3, 5, 6, 8):
        proj.Tasks(i).Duration = "5d"
    for i in (4, 7):
        proj.Tasks(i).Duration = "0d"
        proj.Tasks(i).Milestone = True
    # a small chain so dates spread out
    try:
        proj.Tasks(3).Predecessors = "2"
        proj.Tasks(4).Predecessors = "3"
        proj.Tasks(5).Predecessors = "4"
        proj.Tasks(6).Predecessors = "5"
        proj.Tasks(7).Predecessors = "6"
        proj.Tasks(8).Predecessors = "7"
    except Exception:
        pass
    time.sleep(0.3)
    return proj


# ---------------------------------------------------------------------------
# Fixture variations. Each fn(mp, proj, log) -> dict merged into manifest.json.
# PjTimelineInsertTaskType: 0=bar 1=milestone 2=callout.
# PjTimelineShowHide: TaskOverlaps=0 PanZoom=1 Timescale=2 Today=3
#                     TaskDates=4 TaskProgress=5.
# ---------------------------------------------------------------------------

def v_one_bar_task(mp, proj, log):
    mp.add_bar(log, [2])
    return {"intent": "one task on the timeline as a bar, default range",
            "onTimeline": [2], "display": {"2": "bar"}}


def v_many_bar_tasks(mp, proj, log):
    ids = [2, 3, 5, 6, 8]
    mp.add_bar(log, ids)
    return {"intent": "five tasks, all bars, one timeline bar",
            "onTimeline": ids, "display": {str(i): "bar" for i in ids}}


def v_milestones(mp, proj, log):
    ids = [4, 7]
    mp.add_bar(log, ids)
    return {"intent": "two milestones on the timeline",
            "onTimeline": ids, "milestones": ids}


def v_summary_and_leaf(mp, proj, log):
    # summary task (1) + its children (2,3) + a leaf (5) -- learn how a summary
    # on the timeline is encoded (Timeline Test.mpp showed <t ch="N">).
    mp.add_bar(log, [1, 2, 3, 5])
    return {"intent": "a summary task plus leaves on the timeline",
            "onTimeline": [1, 2, 3, 5], "summaryOnTimeline": 1}


def v_two_bars(mp, proj, log):
    mp.insert_bar(log, 1)
    mp.add_bar(log, [2, 3], bar_index=0)
    mp.add_bar(log, [5, 6], bar_index=1)
    return {"intent": "two timeline bars; tasks 2,3 on bar 0 and 5,6 on bar 1",
            "onTimeline": [2, 3, 5, 6], "bars": 2,
            "barOf": {"2": 0, "3": 0, "5": 1, "6": 1}}


def v_bar_label_range(mp, proj, log):
    mp.insert_bar(log, 1)
    mp.add_bar(log, [2, 3], bar_index=0)
    mp.add_bar(log, [5, 6], bar_index=1)
    mp.bar_label(log, "Near term", 0)
    mp.bar_label(log, "Long range", 1)
    mp.bar_range(log, "1/1/2027", "6/30/2027", 1)
    return {"intent": "two labelled bars; bar 1 has a custom 2027 date range",
            "onTimeline": [2, 3, 5, 6], "bars": 2,
            "barLabels": {"0": "Near term", "1": "Long range"},
            "customRange": {"bar": 1, "start": "2027-01-01", "finish": "2027-06-30"}}


def v_showhide(item, show, key, intent):
    def fn(mp, proj, log):
        mp.add_bar(log, [2, 3, 5])
        mp.view_apply("Timeline")
        mp.show_hide(log, item, show)
        return {"intent": intent, "onTimeline": [2, 3, 5],
                "option": {key: show}}
    return fn


def v_detailed(mp, proj, log):
    mp.add_bar(log, [2, 3, 5])
    mp.view_apply("Timeline")
    mp.timeline_format(log, num_lines=2, minimized=False)
    return {"intent": "TimelineFormat(NumLines=2, Minimized=False) -- detailed",
            "onTimeline": [2, 3, 5], "option": {"minimized": False, "numLines": 2}}


def v_text_styles(mp, proj, log):
    mp.add_bar(log, [2, 3, 4])
    mp.view_apply("Timeline")
    # Item codes with the Timeline view active -- capture whichever apply.
    for item in range(0, 12):
        mp.try_call(log, f"TextStylesEx(item={item})",
                    lambda it=item: mp.app.TextStylesEx(
                        it, "Consolas", 9 + (it % 3), it % 2 == 0, False, False,
                        (it % 6) + 1))
    return {"intent": "timeline text styles set across Item codes 0..11",
            "onTimeline": [2, 3, 4]}


def v_split_shown(mp, proj, log):
    mp.add_bar(log, [2, 3])
    mp.try_call(log, "ViewApply('Gantt with Timeline')",
                lambda: mp.app.ViewApply("Gantt with Timeline"))
    return {"intent": "Timeline shown in the Gantt split view",
            "onTimeline": [2, 3]}


def v_empty(mp, proj, log):
    mp.view_apply("Timeline")
    return {"intent": "Timeline view present, zero tasks on it", "onTimeline": []}


def v_edit_reopen(mp, proj, log):
    mp.add_bar(log, [2, 3])
    mpp, _ = mp.save_outputs("tl_15_edit_then_reopen")
    mp.close_active()
    mp.app.FileOpen(mpp)
    time.sleep(0.5)
    mp.add_bar(log, [5])
    return {"intent": "build/save/reopen/add one task/save again -- shows MS "
                      "Project's own rewrite of the CV_iew pool after an edit",
            "onTimeline": [2, 3, 5], "_alreadySaved": True}


FIXTURES = [
    ("tl_01_one_bar_task", v_one_bar_task),
    ("tl_02_many_bar_tasks", v_many_bar_tasks),
    ("tl_03_milestones", v_milestones),
    ("tl_04_summary_and_leaf", v_summary_and_leaf),
    ("tl_05_two_bars", v_two_bars),
    ("tl_06_bar_label_range", v_bar_label_range),
    ("tl_07_today_off", v_showhide(3, False, "showToday", "Today line hidden")),
    ("tl_08_overlaps_on", v_showhide(0, True, "showOverlaps", "Overlapped tasks on")),
    ("tl_09_panzoom_off", v_showhide(1, False, "showPanZoom", "Pan & Zoom hidden")),
    ("tl_10_timescale_off", v_showhide(2, False, "showTimescale", "Timescale hidden")),
    ("tl_11_detailed", v_detailed),
    ("tl_12_text_styles", v_text_styles),
    ("tl_13_split_shown", v_split_shown),
    ("tl_14_empty_timeline", v_empty),
    ("tl_15_edit_then_reopen", v_edit_reopen),
]


def run_fixture(mp, name, fn):
    log = []
    proj = build_base(mp)
    extra = fn(mp, proj, log) or {}
    manifest = {
        "fixture": name,
        "description": extra.get("intent", ""),
        "note": "Timeline state lives in the .mpp <TLViewData> XML only; MSPDI "
                ".xml carries none of it. This manifest records intent; comLog "
                "records which COM calls actually succeeded.",
        "comLog": log,
    }
    manifest.update({k: v for k, v in extra.items() if not k.startswith("_")})
    if not extra.get("_alreadySaved"):
        mpp, _ = mp.save_outputs(name)
    else:
        mpp = os.path.join(OUT_ROOT, name, name + ".mpp")
        mp.app.FileSaveAs(mpp, PJ_MPP)
    with open(os.path.join(os.path.dirname(mpp), "manifest.json"), "w",
              encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    mp.close_active()
    return "; ".join(log[-3:]) if log else "saved"


def main():
    only = set(sys.argv[1:])          # empty = everything (probe + all fixtures)
    do_probe = (not only) or ("comprobe" in only)
    want = lambda n: (not only) or (n in only)
    results = []
    with MSProjectApp(visible=True) as mp:
        if do_probe:
            try:
                print("[OK]   comprobe:", com_probe(mp))
            except Exception as e:
                print(f"[FAIL] comprobe: {type(e).__name__}: {e}")
                mp.close_active()
        for name, fn in FIXTURES:
            if not want(name):
                continue
            try:
                status = run_fixture(mp, name, fn)
                results.append((name, "OK", status))
                print(f"[OK]   {name}: {status}")
            except Exception as e:
                mp.close_active()
                results.append((name, "ERROR", f"{type(e).__name__}: {e}"))
                print(f"[FAIL] {name}: {type(e).__name__}: {e}")
    print("\nSummary:")
    for name, state, status in results:
        print(f"  {state:5} {name}  {status}")
    # completion marker for the polling driver
    with open(os.path.join(OUT_ROOT, "all.done"), "w", encoding="utf-8") as f:
        f.write(time.strftime("%Y-%m-%d %H:%M:%S") + "\n")
        for name, state, status in results:
            f.write(f"{state}\t{name}\t{status}\n")


if __name__ == "__main__":
    main()

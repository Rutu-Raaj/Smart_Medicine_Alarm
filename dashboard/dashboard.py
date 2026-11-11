import json
import queue
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import messagebox
from urllib.request import urlopen
from urllib.parse import urlencode

try:
    import customtkinter as ctk
except ImportError:
    raise SystemExit(
        "CustomTkinter is not installed. Install it with:\n"
        "    pip install customtkinter"
    )

# Set True if "Device Time" shows UTC instead of local time
# (the firmware has no timezone configured). Doses then fire on time.
SEND_LOCAL_TIME = False

REFRESH_MS = 5000
RESYNC_MS = 60 * 60 * 1000  # re-send laptop time hourly to correct RTC drift

# Colour tokens as (light, dark) pairs; CustomTkinter picks by Windows theme.
BG = ("#EDF1F0", "#121819")
SURFACE = ("#FFFFFF", "#1A2224")
LINE = ("#D3DCDA", "#2A3538")
INK = ("#1C2B2E", "#E2EBE9")
MUTED = ("#5A6B6F", "#94A5A8")
ACCENT = ("#2F6F5E", "#74C3A8")
ACCENT_HOVER = ("#255A4C", "#8FD3BB")
ON_ACCENT = ("#FFFFFF", "#0E201B")
TAKEN_BG = ("#E1EEE9", "#1D3029")
ALERT = ("#B3261E", "#FF8A80")
ALERT_BG = ("#FBE4E1", "#3A1B19")
ALERT_HOVER = ("#F6D3CE", "#4A2220")

HOURS = [f"{h:02d}" for h in range(24)]
MINUTES = [f"{m:02d}" for m in range(0, 60, 5)]  # any minute can be typed


class SmartMedicineDashboard:
    def __init__(self, root):
        self.root = root
        self.root.title("Smart Medicine")
        self.root.geometry("960x720")  # fits 1366x768 laptop screens
        self.root.minsize(800, 660)
        self.root.configure(fg_color=BG)

        self.base_url = tk.StringVar(value="http://192.168.4.1")
        self.connection_text = tk.StringVar(value="Connecting to the device...")
        self.status_text = tk.StringVar(value="--")
        self.time_text = tk.StringVar(value="--:--")
        self.next_text = tk.StringVar(value="--")
        self.slots_text = tk.StringVar(value="--")
        self.clients_text = tk.StringVar(value="--")
        self.clock_text = tk.StringVar(value="Not set")
        self.sensor_text = tk.StringVar(value="Idle")
        self.servo_text = tk.StringVar(value="--")
        self.tablet_text = tk.StringVar(value="")
        self.updated_text = tk.StringVar(value="Not updated yet")

        self.dose_vars = []
        self.tray = []
        self.schedule_dirty = False
        self.loading_schedule = False
        self.online = False
        self.alarm_was_active = False

        # One worker thread runs requests in order, so none are dropped.
        self.jobs = queue.Queue()
        threading.Thread(target=self._worker, daemon=True).start()

        self._build_ui()

        self.root.after(500, self.refresh)
        self.root.after(1500, self.sync_time_silently)
        self.root.after(REFRESH_MS, self.auto_refresh)

    # ---------------- USER INTERFACE ----------------

    def _fonts(self):
        families = set(tkfont.families(self.root))
        display = (
            "Segoe UI Variable Display"
            if "Segoe UI Variable Display" in families
            else "Segoe UI"
        )
        body = "Segoe UI"

        def f(family, size, weight="normal"):
            return ctk.CTkFont(family=family, size=size, weight=weight)

        self.f_title = f(display, 22, "bold")
        self.f_clock = f(display, 40, "bold")
        self.f_value = f(display, 22, "bold")
        self.f_tray_time = f(display, 26, "bold")
        self.f_heading = f(body, 16, "bold")
        self.f_body = f(body, 14)
        self.f_body_bold = f(body, 14, "bold")
        self.f_small = f(body, 12)

    def _label(self, parent, size="body", color=INK, **kw):
        font = {
            "body": self.f_body,
            "bold": self.f_body_bold,
            "small": self.f_small,
            "heading": self.f_heading,
            "value": self.f_value,
        }[size]
        return ctk.CTkLabel(
            parent, font=font, text_color=color,
            anchor="w", justify="left", **kw
        )

    def _button(self, parent, text, command, kind="primary"):
        styles = {
            "primary": dict(fg_color=ACCENT, hover_color=ACCENT_HOVER,
                            text_color=ON_ACCENT, border_width=0),
            "secondary": dict(fg_color="transparent", hover_color=LINE,
                              text_color=INK, border_width=1,
                              border_color=LINE),
            "danger": dict(fg_color="transparent", hover_color=ALERT_HOVER,
                           text_color=ALERT, border_width=1,
                           border_color=ALERT),
        }[kind]
        return ctk.CTkButton(
            parent, text=text, command=command, height=40,
            corner_radius=10, font=self.f_body_bold, **styles
        )

    def _build_ui(self):
        self._fonts()

        # Status bar: packed first at the bottom so it never gets clipped.
        footer = ctk.CTkFrame(self.root, fg_color="transparent")
        footer.pack(side="bottom", fill="x", padx=28, pady=(8, 12))
        self._label(footer, "small", MUTED,
                    textvariable=self.connection_text).pack(side="left")
        self._label(footer, "small", MUTED,
                    textvariable=self.updated_text).pack(side="right")

        main = ctk.CTkFrame(self.root, fg_color="transparent")
        main.pack(fill="both", expand=True, padx=28, pady=(20, 0))
        main.grid_columnconfigure(0, weight=1)

        # Header: name left, live connection state right.
        header = ctk.CTkFrame(main, fg_color="transparent")
        header.grid(row=0, column=0, sticky="ew")
        header.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(
            header, text="Smart Medicine", font=self.f_title,
            text_color=INK, anchor="w"
        ).grid(row=0, column=0, sticky="w")

        self.conn_label = ctk.CTkLabel(
            header, text="●  Connecting", font=self.f_body_bold,
            text_color=MUTED
        )
        self.conn_label.grid(row=0, column=1, sticky="e")

        # Alarm banner: while the buzzer sounds it takes the overview's
        # place (same row, similar height), so the tray below never jumps.
        self.alarm_banner = ctk.CTkFrame(
            main, fg_color=ALERT_BG, border_color=ALERT,
            border_width=2, corner_radius=14
        )
        self.alarm_banner.grid(row=2, column=0, sticky="ew", pady=(14, 0))
        ctk.CTkLabel(
            self.alarm_banner, text="Time to take medicine",
            font=self.f_value, text_color=ALERT, anchor="w"
        ).pack(anchor="w", padx=20, pady=(14, 0))
        ctk.CTkLabel(
            self.alarm_banner, textvariable=self.tablet_text,
            font=self.f_body, text_color=INK, anchor="w"
        ).pack(anchor="w", padx=20, pady=(0, 14))
        self.alarm_banner.grid_remove()

        # Overview: device time, next dose, slots left.
        overview = self.overview = ctk.CTkFrame(main, fg_color="transparent")
        overview.grid(row=2, column=0, sticky="ew", pady=(14, 0))

        clock = ctk.CTkFrame(overview, fg_color="transparent")
        clock.pack(side="left", anchor="s")
        self._label(clock, "small", MUTED, text="Device time").pack(anchor="w")
        ctk.CTkLabel(
            clock, textvariable=self.time_text, font=self.f_clock,
            text_color=INK, anchor="w", height=50
        ).pack(anchor="w")

        for title, var in (("Slots left", self.slots_text),
                           ("Next dose", self.next_text)):
            block = ctk.CTkFrame(overview, fg_color="transparent")
            block.pack(side="right", anchor="s", padx=(36, 0), pady=(0, 6))
            self._label(block, "small", MUTED, text=title).pack(anchor="w")
            self._label(block, "value", INK, textvariable=var).pack(anchor="w")

        # Pill tray: one compartment per dose, coloured by today's state.
        tray = ctk.CTkFrame(main, fg_color="transparent")
        tray.grid(row=3, column=0, sticky="ew", pady=(14, 0))

        for i in range(3):
            tray.grid_columnconfigure(i, weight=1, uniform="tray")
            box = ctk.CTkFrame(
                tray, fg_color=SURFACE, border_color=LINE,
                border_width=2, corner_radius=16
            )
            box.grid(row=0, column=i, sticky="nsew",
                     padx=(0 if i == 0 else 12, 0))
            self._label(box, "small", MUTED, text=f"Dose {i + 1}").pack(
                anchor="w", padx=18, pady=(12, 0)
            )
            time_lbl = ctk.CTkLabel(
                box, text="--:--", font=self.f_tray_time,
                text_color=INK, anchor="w"
            )
            time_lbl.pack(anchor="w", padx=18)
            state_lbl = self._label(box, "bold", MUTED,
                                    text="Waiting for device")
            state_lbl.pack(anchor="w", padx=18, pady=(0, 12))
            self.tray.append((box, time_lbl, state_lbl))

        # Lower area: schedule editor and device details.
        lower = ctk.CTkFrame(main, fg_color="transparent")
        lower.grid(row=4, column=0, sticky="nsew", pady=(14, 0))
        main.grid_rowconfigure(4, weight=1)
        lower.grid_columnconfigure((0, 1), weight=1, uniform="lower")

        self._build_schedule(lower)
        self._build_device(lower)

    def _build_schedule(self, parent):
        panel = ctk.CTkFrame(parent, fg_color=SURFACE, corner_radius=16)
        panel.grid(row=0, column=0, sticky="nsew", padx=(0, 12))

        self._label(panel, "heading", INK, text="Daily schedule").pack(
            anchor="w", padx=22, pady=(20, 0)
        )
        self._label(
            panel, "small", MUTED,
            text="24-hour time. Saving also resets today's dose count."
        ).pack(anchor="w", padx=22, pady=(2, 10))

        rows = ctk.CTkFrame(panel, fg_color="transparent")
        rows.pack(anchor="w", padx=22)

        combo = dict(
            width=84, height=38, corner_radius=8, font=self.f_body,
            dropdown_font=self.f_body, fg_color=BG, border_color=LINE,
            button_color=LINE, button_hover_color=MUTED, text_color=INK,
            dropdown_fg_color=SURFACE, dropdown_text_color=INK,
            dropdown_hover_color=TAKEN_BG,
        )

        for i in range(3):
            hour = tk.StringVar(value=["08", "14", "20"][i])
            minute = tk.StringVar(value="00")

            self._label(rows, "bold", INK, text=f"Dose {i + 1}",
                        width=80).grid(row=i, column=0, sticky="w", pady=4)
            ctk.CTkComboBox(rows, values=HOURS, variable=hour,
                            **combo).grid(row=i, column=1, pady=4)
            self._label(rows, "bold", MUTED, text=":").grid(
                row=i, column=2, padx=8
            )
            ctk.CTkComboBox(rows, values=MINUTES, variable=minute,
                            **combo).grid(row=i, column=3, pady=4)

            hour.trace_add("write", lambda *_: self._mark_schedule_dirty())
            minute.trace_add("write", lambda *_: self._mark_schedule_dirty())

            self.dose_vars.append((hour, minute))

        self.schedule_msg = self._label(panel, "bold", MUTED, text="")
        self.schedule_msg.pack(anchor="w", padx=22, pady=(6, 0))

        actions = ctk.CTkFrame(panel, fg_color="transparent")
        actions.pack(anchor="w", padx=22, pady=(4, 20))
        self._button(actions, "Save schedule", self.save_schedule).pack(
            side="left"
        )
        self._button(actions, "Reset doses", self.reset_doses,
                     "danger").pack(side="left", padx=(10, 0))

    def _build_device(self, parent):
        panel = ctk.CTkFrame(parent, fg_color=SURFACE, corner_radius=16)
        panel.grid(row=0, column=1, sticky="nsew")

        self._label(panel, "heading", INK, text="Device").pack(
            anchor="w", padx=22, pady=(20, 8)
        )

        facts = ctk.CTkFrame(panel, fg_color="transparent")
        facts.pack(fill="x", padx=22)
        facts.grid_columnconfigure(1, weight=1)

        for row, (title, var) in enumerate((
            ("Status", self.status_text),
            ("Clock", self.clock_text),
            ("Tablet sensor", self.sensor_text),
            ("Servo angle", self.servo_text),
            ("Connected devices", self.clients_text),
        )):
            self._label(facts, "body", MUTED, text=title).grid(
                row=row, column=0, sticky="w", pady=1
            )
            ctk.CTkLabel(
                facts, textvariable=var, font=self.f_body_bold,
                text_color=INK, anchor="e"
            ).grid(row=row, column=1, columnspan=1 if row == 1 else 2,
                   sticky="e", pady=1)

        # Manual clock sync sits beside the clock state it changes.
        ctk.CTkButton(
            facts, text="Sync", command=self.sync_time, width=56,
            height=26, corner_radius=8, font=self.f_small,
            fg_color="transparent", hover_color=LINE, text_color=INK,
            border_width=1, border_color=LINE
        ).grid(row=1, column=2, sticky="e", padx=(10, 0), pady=1)

        self._label(panel, "small", MUTED, text="Device address").pack(
            anchor="w", padx=22, pady=(14, 2)
        )
        address = ctk.CTkFrame(panel, fg_color="transparent")
        address.pack(fill="x", padx=22, pady=(0, 20))
        address.grid_columnconfigure(0, weight=1)
        ctk.CTkEntry(
            address, textvariable=self.base_url, height=38,
            corner_radius=8, font=self.f_body, fg_color=BG,
            border_color=LINE, text_color=INK
        ).grid(row=0, column=0, sticky="ew", padx=(0, 8))
        self._button(address, "Connect", self.refresh,
                     "secondary").grid(row=0, column=1)

    def _render_online(self):
        if self.online:
            self.conn_label.configure(text="●  Connected",
                                      text_color=ACCENT)
        else:
            self.conn_label.configure(text="●  Offline",
                                      text_color=ALERT)

    # ---------------- HTTP REQUESTS ------------------

    def _request(self, path, params=None, message=None,
                 on_ok=None, background=False):
        # Build the URL here, on the Tk thread; the worker never touches Tk.
        base = self.base_url.get().strip().rstrip("/")

        if not base.startswith(("http://", "https://")):
            base = "http://" + base

        url = base + path

        if params:
            url += "?" + urlencode(params)

        if not background:
            self.connection_text.set("Sending...")

        self.jobs.put((url, message, on_ok))

    def _worker(self):
        while True:
            url, message, on_ok = self.jobs.get()
            result = error = None

            try:
                with urlopen(url, timeout=4) as response:
                    body = response.read().decode("utf-8")
            except Exception:
                error = (
                    "Can't reach the device. Connect this laptop to the "
                    "Smart_Medicine_Alarm Wi-Fi, then select Connect."
                )
            else:
                try:
                    result = json.loads(body)
                except ValueError:
                    error = "Device sent an invalid (non-JSON) response"

            self.root.after(0, self._finish, result, error, message, on_ok)

    def _finish(self, result, error, message, on_ok):
        if error:
            self.online = False
            self._render_online()
            self.connection_text.set(error)
            self.status_text.set("Offline")
            return

        if isinstance(result, dict) and result.get("ok") is False:
            self.connection_text.set(
                "Device error: "
                + str(result.get("error", "Unknown error"))
            )
            return

        if message:
            self.connection_text.set(message)
        elif not self.online:
            self.connection_text.set("Connected to the device")

        self.online = True
        self._render_online()

        if on_ok:
            on_ok()

        if isinstance(result, dict) and "status" in result:
            self._apply_status(result)

    # ---------------- STATUS REFRESH -----------------

    def refresh(self, background=False):
        self._request(
            "/api/status",
            message=None if background else "Connected to the device",
            background=background
        )

    def _apply_status(self, data):
        self.updated_text.set("Updated " + time.strftime("%H:%M:%S"))
        self.status_text.set(str(data.get("status", "--")))
        self.next_text.set(str(data.get("next_dose", "--")))
        self.slots_text.set(f'{data.get("slots_left", "--")} of 3')
        self.clients_text.set(str(data.get("client_count", "--")))
        self.servo_text.set(f'{data.get("servo_angle", "--")}°')

        clock_set = bool(data.get("clock_set"))
        self.time_text.set(
            str(data.get("current_time", "--:--")) if clock_set else "--:--"
        )

        if clock_set:
            self.clock_text.set("Synchronized")
        else:
            # ESP32 rebooted or never synced: doses won't fire until set.
            self.clock_text.set("Not set, sending time")
            self._sync("Device clock was not set. Laptop time sent.")

        alarm_active = bool(data.get("alarm_active", False))
        tablet_seen = bool(data.get("tablet_seen", False))

        if alarm_active:
            self.tablet_text.set(
                "Tablets detected. Take them from the tray to stop the alarm."
                if tablet_seen else
                "Waiting for the sensor to detect the tablets."
            )
            self.sensor_text.set(
                "Tablets detected" if tablet_seen else "Waiting for tablets"
            )
            self.overview.grid_remove()
            self.alarm_banner.grid()

            if not self.alarm_was_active:
                self.root.bell()
                self.root.deiconify()
                self.root.lift()
        else:
            self.sensor_text.set("Idle")
            self.alarm_banner.grid_remove()
            self.overview.grid()

        self.alarm_was_active = alarm_active

        doses = data.get("doses", [])

        # Follow the device schedule unless the user has unsaved edits.
        if not self.schedule_dirty:
            self.loading_schedule = True
            for i, dose in enumerate(doses[:3]):
                hour_var, minute_var = self.dose_vars[i]
                hour_var.set(f'{int(dose.get("hour", 0)):02d}')
                minute_var.set(f'{int(dose.get("minute", 0)):02d}')
            self.loading_schedule = False

        self._render_tray(
            doses[:3],
            alarm_active,
            str(data.get("next_dose", "")),
            self._minutes(data.get("current_time")) if clock_set else None,
        )

    @staticmethod
    def _minutes(text):
        """'02:05 PM' -> 845 minutes after midnight, or None."""
        try:
            t = time.strptime(str(text).strip(), "%I:%M %p")
        except ValueError:
            return None
        return t.tm_hour * 60 + t.tm_min

    def _render_tray(self, doses, alarm_active, next_dose, now):
        # While the alarm rings, the latest dispensed dose is the one to take.
        take_now = None
        if alarm_active:
            done = [
                (self._minutes(d.get("time")), i)
                for i, d in enumerate(doses) if d.get("done")
            ]
            due = [x for x in done if x[0] is not None
                   and (now is None or x[0] <= now)]
            if due or done:
                take_now = max(due or done)[1]

        for i, (box, time_lbl, state_lbl) in enumerate(self.tray):
            if i >= len(doses):
                continue

            dose = doses[i]
            dose_time = str(dose.get("time", "--:--"))
            minutes = self._minutes(dose_time)
            time_lbl.configure(text=dose_time)

            if i == take_now:
                state, bg, border, color = "Take now", ALERT_BG, ALERT, ALERT
            elif dose.get("done"):
                state, bg, border, color = (
                    "Dispensed", TAKEN_BG, TAKEN_BG, ACCENT
                )
            elif dose_time == next_dose:
                state, bg, border, color = "Next", SURFACE, ACCENT, ACCENT
            elif now is not None and minutes is not None and minutes < now:
                state, bg, border, color = "Missed", SURFACE, LINE, ALERT
            else:
                state, bg, border, color = "Upcoming", SURFACE, LINE, MUTED

            box.configure(fg_color=bg, border_color=border)
            state_lbl.configure(text=state, text_color=color)

    def _mark_schedule_dirty(self):
        if self.loading_schedule:
            return
        self.schedule_dirty = True
        self.schedule_msg.configure(text="Unsaved changes", text_color=MUTED)

    def auto_refresh(self):
        # Skip if requests are already waiting (device slow or offline).
        if self.jobs.empty():
            self.refresh(background=True)

        self.root.after(REFRESH_MS, self.auto_refresh)

    # ---------------- DEVICE CLOCK ------------------

    def _sync(self, message):
        epoch = int(time.time())

        if SEND_LOCAL_TIME:
            epoch += time.localtime().tm_gmtoff

        self._request(
            "/api/settime",
            {"epoch": epoch},
            message=message,
            background=True
        )

    def sync_time_silently(self):
        self._sync("Laptop time sent to the device")
        self.root.after(RESYNC_MS, self.sync_time_silently)

    def sync_time(self):
        self._sync("Device clock synchronized from this laptop")

    # ---------------- SCHEDULE -----------------------

    def save_schedule(self):
        params = {}
        times = set()

        for i, (hour_var, minute_var) in enumerate(self.dose_vars, start=1):
            try:
                hour = int(hour_var.get())
                minute = int(minute_var.get())
            except ValueError:
                return self._schedule_error(
                    f"Dose {i}: hour and minute must be whole numbers."
                )

            if not 0 <= hour <= 23:
                return self._schedule_error(
                    f"Dose {i}: hour must be between 0 and 23."
                )

            if not 0 <= minute <= 59:
                return self._schedule_error(
                    f"Dose {i}: minute must be between 0 and 59."
                )

            if (hour, minute) in times:
                return self._schedule_error(
                    f"Dose {i} has the same time as another dose."
                )

            times.add((hour, minute))
            params[f"h{i}"] = hour
            params[f"m{i}"] = minute

        def saved():
            self.schedule_dirty = False
            self.schedule_msg.configure(text="Schedule saved",
                                        text_color=ACCENT)

        # Edits stay "unsaved" until the device confirms the save.
        self._request(
            "/api/schedule",
            params,
            message="Schedule saved on the device",
            on_ok=saved
        )

    def _schedule_error(self, text):
        self.schedule_msg.configure(text=text, text_color=ALERT)

    # ---------------- RESET --------------------------

    def reset_doses(self):
        confirmed = messagebox.askyesno(
            "Reset today's doses",
            "Mark all of today's doses as pending and return the servo "
            "to the start position?"
        )

        if not confirmed:
            return

        self._request(
            "/api/reset",
            message="Today's doses reset"
        )


if __name__ == "__main__":
    ctk.set_appearance_mode("system")
    root = ctk.CTk()
    app = SmartMedicineDashboard(root)
    root.mainloop()

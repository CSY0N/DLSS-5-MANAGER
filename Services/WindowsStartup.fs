namespace DLSS_5_MANAGER.Services

open System
open System.IO
open Microsoft.Win32

/// Settings -> Features -> Start with Windows: one value under the user's own
/// Run key, pointing at this executable. Nothing machine-wide - no service, no
/// scheduled task. The switch in Settings is the only thing that writes it,
/// and the uninstaller takes it away (DLSS5Installer.iss, [Registry]).
module WindowsStartup =

    let private runKey = @"Software\Microsoft\Windows\CurrentVersion\Run"

    let valueName = "DLSS 5 MANAGER"

    /// What Windows passes at sign-in: the window starts minimized - or in the
    /// tray, with Minimize to tray on - instead of in the user's face.
    let startupArgument = "--startup"

    let private command () =
        let exe = Environment.ProcessPath

        if String.IsNullOrWhiteSpace(exe) then "" else "\"" + exe + "\" " + startupArgument

    let private current () =
        try
            use key = Registry.CurrentUser.OpenSubKey(runKey)

            if isNull key then
                ""
            else
                match key.GetValue(valueName) with
                | :? string as value -> value
                | _ -> ""
        with _ ->
            ""

    /// The executable a Run value starts, without its quotes and arguments.
    let private exeOf (value: string) =
        let v = value.Trim()

        if v.StartsWith("\"") then
            let close = v.IndexOf('"', 1)
            if close > 1 then v.Substring(1, close - 1) else ""
        else
            v.Split(' ').[0]

    /// Windows keeps its own word on every Run entry: Task Manager and
    /// Settings -> Startup apps write it here, twelve bytes whose first is even
    /// for "enabled" and odd for "disabled". An entry switched off there stays
    /// in the Run key and is simply never started - the switch in the program
    /// said on, and at sign-in nothing came (round 30).
    let private approvedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"

    let private disabledByWindows () =
        try
            use key = Registry.CurrentUser.OpenSubKey(approvedKey)

            if isNull key then
                false
            else
                match key.GetValue(valueName) with
                | :? (byte[]) as bytes when bytes.Length > 0 -> bytes[0] &&& 1uy = 1uy
                | _ -> false
        with _ ->
            false

    /// On only when the entry is there and Windows has not switched it off.
    let isEnabled () = current () <> "" && not (disabledByWindows ())

    /// Takes Windows' "disabled" off the entry: turning the switch on in the
    /// program is the person saying it should start.
    let private approve () =
        try
            use key = Registry.CurrentUser.OpenSubKey(approvedKey, true)
            if not (isNull key) then key.DeleteValue(valueName, false)
        with _ ->
            ()

    /// Turns it on or off. False when Windows refused - the switch then says
    /// what is really there on its next read.
    let set (on: bool) : bool =
        try
            use key = Registry.CurrentUser.CreateSubKey(runKey)

            if on then
                let cmd = command ()

                if cmd = "" then
                    false
                else
                    key.SetValue(valueName, cmd)
                    approve ()
                    true
            else
                key.DeleteValue(valueName, false)
                true
        with _ ->
            false

    /// After an update moved the program, an entry the user turned on points
    /// at a file that is gone - it is pointed at this one. Only then: a copy
    /// run from somewhere else never takes over an entry that still works.
    let refresh () =
        let now = current ()

        if now <> "" then
            let exe = exeOf now

            if exe = "" || not (File.Exists(exe)) then set true |> ignore

    /// One line per start by Windows, in the program's own data folder: when
    /// a person says it never came up at sign-in, this says whether Windows
    /// started it at all, and what happened next.
    let note (text: string) =
        try
            let dir =
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DLSS5Manager")

            Directory.CreateDirectory(dir) |> ignore
            let path = Path.Combine(dir, "startup.log")
            // A few hundred lines at most: it is a note, not a history.
            if File.Exists(path) && FileInfo(path).Length > 64L * 1024L then File.Delete(path)
            File.AppendAllText(path, sprintf "%s  %s%s" (DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss")) text Environment.NewLine)
        with _ ->
            ()

    let launchedAtStartup () =
        Environment.GetCommandLineArgs()
        |> Array.exists (fun a -> String.Equals(a, startupArgument, StringComparison.OrdinalIgnoreCase))

namespace DLSS_5_MANAGER.ViewModels

open System
open System.Diagnostics
open System.IO
open System.Text
open System.Text.Json.Nodes
open System.Threading.Tasks
open Avalonia.Threading
open Avalonia.Media
open CommunityToolkit.Mvvm.Input
open DLSS_5_MANAGER.Services

/// Owns only the embedded Screen session. The games installer never uses this state.
type ScreenViewModel() as this =
    inherit ViewModelBase()

    let dataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DLSS5Manager", "Screen")
    let mutable child: Process option = None
    let mutable state = JsonObject()
    let mutable ready = false
    let mutable stopping = false
    let mutable preparing = false
    let mutable preparingSince = DateTime.MinValue
    let mutable status = "Preparing Screen…"
    let mutable error = ""
    let mutable updating = false
    let mutable presetName = ""
    let mutable selectedPreset = ""
    let mutable query = ""
    let noMicrophones = [| "None" |]
    let recordingRates = [| "60 FPS"; "30 FPS" |]
    let models = [| "Default"; "Natural"; "Cinematic" |]
    let flowMultipliers = [| "×2"; "×3"; "×4"; "×5"; "×6" |]
    let fgResolutions = [| "Native"; "1080p"; "720p" |]
    /// The count before the window in front is taken, seconds - Lossless
    /// Scaling's own five (round 30: the list of counts went with the Source card).
    let sourceCount = 5
    // The folded cards of the page (round 29): which are open is the
    // person's, kept in its own file like the shortcuts. Everything starts
    // folded - the page reads as the sketch it was laid out from.
    let sectionsConfig = Path.Combine(dataDir, "sections.json")
    let openSections = System.Collections.Generic.HashSet<string>()
    let sectionNames = [ "effect", "Effect"; "colors", "Colors"; "fps", "Fps"; "presets", "Presets"
                         "settings", "Settings"; "capture", "Capture"; "overlay", "Overlay"; "smfg", "Smfg" ]
    let saveSections () =
        try
            Directory.CreateDirectory(dataDir) |> ignore
            let opened = JsonArray()
            for key in openSections do opened.Add(JsonValue.Create(key))
            let cfg = JsonObject()
            cfg["open"] <- opened
            File.WriteAllText(sectionsConfig, cfg.ToJsonString())
        with _ -> ()
    // Lists that carry a translated word: rebuilt only when the language
    // changes, so each picker keeps its item.
    let mutable mfgOptions: string array = [||]
    let mutable colorSpaces: string array = [||]
    let mutable listsFor = ""
    /// FLOW MFG is not offered on RTX cards (round 29): one that was left on
    /// from before is switched off, once per time it is seen.
    let mutable flowOffAsked = DateTime.MinValue
    let mutable workerSeen = 0                // LIVE FLOW's worker process (round 42)
    let passOptions =
        [| "1 · Standard"; "2 · Multi-pass"; "3 · Multi-pass"; "4 · Multi-pass"; "5 · Multi-pass"; "6 · Multi-pass" |]
    // The two motion backends as people read them, in the language in use -
    // rebuilt only when the language changes, so the picker keeps its item.
    let mutable motionLabels: string array = [||]
    let mutable motionLabelsFor = ""
    /// AMD MODE is on in Settings: LIVE FLOW runs its Radeon version (ALPHA) -
    /// its own worker, the Radeon network in place of NGX, nothing NVIDIA.
    let mutable amdMode = false
    let mutable outputFolder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "DLSS 5 MANAGER")
    let mutable microphone = "None"
    let mutable systemAudio = true
    let mutable recordingFps = 60
    let captureConfig = Path.Combine(dataDir, "capture.json")
    let saveCapture () =
        try
            Directory.CreateDirectory(dataDir) |> ignore
            let cfg = JsonObject()
            cfg["folder"] <- JsonValue.Create(outputFolder)
            cfg["microphone"] <- JsonValue.Create(microphone)
            cfg["system_audio"] <- JsonValue.Create(systemAudio)
            cfg["fps"] <- JsonValue.Create(recordingFps)
            File.WriteAllText(captureConfig, cfg.ToJsonString())
        with _ -> ()
    // ---- shortcuts -------------------------------------------------------
    // Kept in their own file beside capture.json, so a bad value can never
    // take the capture settings down with it.
    let hotkeysConfig = Path.Combine(dataDir, "hotkeys.json")
    let mutable overlayMods = 4u    // Shift
    let mutable overlayKey = 0x4Fu  // O
    let mutable dlssMods = 0u
    let mutable dlssKey = 0x75u     // F6
    // The screenshot's own shortcut (round 30): the Screenshot button, from
    // inside the game.
    let mutable shotMods = 0u
    let mutable shotKey = 0x79u     // F10
    // The recording's own shortcut (round 34): Record / Stop recording from
    // inside the game. Shift+F9 - Alt+F9 and Alt+F10 are NVIDIA's recorder's.
    let mutable recMods = 4u        // Shift
    let mutable recKey = 0x78u      // F9
    let mutable listening = ""
    let hotkeysChanged = Event<unit>()

    let saveHotkeys () =
        try
            Directory.CreateDirectory(dataDir) |> ignore
            let cfg = JsonObject()
            cfg["overlay_mods"] <- JsonValue.Create(overlayMods)
            cfg["overlay_key"] <- JsonValue.Create(overlayKey)
            cfg["dlss_mods"] <- JsonValue.Create(dlssMods)
            cfg["dlss_key"] <- JsonValue.Create(dlssKey)
            cfg["shot_mods"] <- JsonValue.Create(shotMods)
            cfg["shot_key"] <- JsonValue.Create(shotKey)
            cfg["rec_mods"] <- JsonValue.Create(recMods)
            cfg["rec_key"] <- JsonValue.Create(recKey)
            File.WriteAllText(hotkeysConfig, cfg.ToJsonString())
        with _ -> ()

    /// "Ctrl+Shift+O", "F6" - the way people write a shortcut down.
    let keyText (mods: uint32) (vk: uint32) =
        let k = int vk

        let name =
            if k >= 0x41 && k <= 0x5A then string (char k)
            elif k >= 0x30 && k <= 0x39 then string (char k)
            elif k >= 0x60 && k <= 0x69 then sprintf "Num %d" (k - 0x60)
            elif k >= 0x70 && k <= 0x87 then sprintf "F%d" (k - 0x6F)
            else sprintf "Key %02X" k

        String.Join(
            "+",
            [ if mods &&& 2u <> 0u then yield "Ctrl"
              if mods &&& 1u <> 0u then yield "Alt"
              if mods &&& 4u <> 0u then yield "Shift"
              if mods &&& 8u <> 0u then yield "Win"
              yield name ]
        )

    // ---- FPS counter ------------------------------------------------------
    // On or off is the user's, kept in its own file like the shortcuts. The
    // figures arrive with every state record (about 2.5 a second): the base
    // rate the pipeline runs at, and - with FLOW MFG on - the rate the screen
    // really shows.
    let counterConfig = Path.Combine(dataDir, "counter.json")
    let mutable counterOn = false
    let mutable liveFps = Double.NaN
    let mutable displayFps = Double.NaN
    let mutable counterMode = ""
    let mutable counterArea = (0, 0, 0, 0)
    let mutable counterAt = DateTime.MinValue
    /// Where the counter sits, from the output screen's top-left corner in
    /// pixels. Moved by dragging it while the overlay is open.
    let mutable counterOffset = (20, 20)
    /// The counter itself is drawn by the worker, in the frame (round 28,
    /// lf_counter.inl) - it reads this file. Only while the overlay is open
    /// does the manager's own small window stand in for it, so it can be
    /// dragged: "hold" tells the worker to leave it out for that long.
    let mutable counterHeld = false
    let nrModelsList = [| Localization.current.LfNrModelQuality; Localization.current.LfNrModelFast |]
    // Round 37: "Select window" is a choice again, off unless turned on. Off, LIVE FLOW and MFG take the whole
    // screen, as since round 33; on, the button at the top picks a window, and DLSS 5 and MFG both take it.
    let sourceConfig = Path.Combine(dataDir, "source.json")
    let mutable windowPick = false
    let saveSource () =
        try
            Directory.CreateDirectory(dataDir) |> ignore
            let cfg = JsonObject()
            cfg["pick"] <- JsonValue.Create(windowPick)
            File.WriteAllText(sourceConfig, cfg.ToJsonString())
        with _ -> ()

    let saveCounter () =
        try
            Directory.CreateDirectory(dataDir) |> ignore
            let cfg = JsonObject()
            cfg["on"] <- JsonValue.Create(counterOn)
            cfg["dx"] <- JsonValue.Create(fst counterOffset)
            cfg["dy"] <- JsonValue.Create(snd counterOffset)
            cfg["hold"] <- JsonValue.Create(counterHeld)
            File.WriteAllText(counterConfig, cfg.ToJsonString())
        with _ -> ()

    let mutable lastSound = DateTime.MinValue
    let sound () =
        if (DateTime.UtcNow-lastSound).TotalMilliseconds >= 90. then
            lastSound <- DateTime.UtcNow
            UiSounds.tick()
    let pendingNumbers = System.Collections.Generic.Dictionary<string, float>()
    let numberTimer = DispatcherTimer(Interval=TimeSpan.FromMilliseconds(220.))
    /// What the hand set last, and when. The engine answers every 0.4 s with
    /// the state it had at that moment - which, just after a drag, is the
    /// state from BEFORE the drag reached it. Taking that answer at its word
    /// pulled the bar back under the hand and then let it jump forward again.
    /// For a moment after each edit the hand's own value wins.
    let recentEdits = System.Collections.Generic.Dictionary<string, float * DateTime>()
    /// The comparison wipe travels on its own path. Every other slider waits
    /// until the hand has been still for 220 ms - right for a setting that
    /// rebuilds something, but the wipe is read off every frame the engine
    /// sends, so the wait only meant the divider stood still until the drag
    /// was over. It goes out at most every 33 ms instead, the latest position
    /// always, and the bar on screen follows the hand without waiting at all.
    let splitTimer = DispatcherTimer(Interval=TimeSpan.FromMilliseconds(33.))
    let mutable lastSplitSent = DateTime.MinValue
    let mutable splitPending = Double.NaN
    let matches (words: string) =
        let text = words.ToLowerInvariant()
        query.ToLowerInvariant().Split(' ', StringSplitOptions.RemoveEmptyEntries)
        |> Array.forall text.Contains
    let lists = System.Collections.Generic.Dictionary<string, string array>()

    // The window list with "None" in front, rebuilt only when the list behind
    // it changes - a new array on every read would make the box re-select.
    let mutable lastWindows: string array = [||]
    let mutable windowsWithNone: string array = [||]

    let str (key: string) fallback =
        match state[key] with
        | null -> fallback
        | value -> try value.GetValue<string>() with _ -> fallback
    let number (key: string) fallback =
        match state[key] with
        | null -> fallback
        | value -> try value.GetValue<float>() with _ -> fallback
    let flag (key: string) fallback =
        match state[key] with
        | null -> fallback
        | value -> try value.GetValue<bool>() with _ -> fallback
    let items (key: string) =
        let next =
            match state[key] with
            | :? JsonArray as values -> values |> Seq.map (fun v -> v.GetValue<string>()) |> Seq.toArray
            | _ -> [||]
        match lists.TryGetValue(key) with
        | true, previous when previous = next -> previous
        | _ -> lists[key] <- next; next
    let notify () = this.RaisePropertyChanged("")
    /// Which generator the card gets (round 29). An RTX card: DLSS MFG, and
    /// FLOW MFG is not offered at all. AMD MODE, or a card that is not an RTX
    /// (a GTX, an Arc): FLOW MFG in its place - DLSS MFG cannot run there.
    /// Until the engine has named the card, the page shows the RTX side.
    let usesFlow () =
        let card = str "gpu_text" ""
        amdMode || (card <> "" && not (card.Contains("RTX", StringComparison.OrdinalIgnoreCase)))
    let setError text =
        error <- text
        this.RaisePropertyChanged("Error")
        this.RaisePropertyChanged("HasError")
    let send (message: JsonObject) =
        match child with
        | Some p when not p.HasExited ->
            try p.StandardInput.WriteLine(message.ToJsonString()); p.StandardInput.Flush()
            with ex -> setError ex.Message
        | _ -> ()
    let flushNumbers () =
        numberTimer.Stop()
        let values = pendingNumbers |> Seq.map (fun p -> p.Key,p.Value) |> Seq.toArray
        pendingNumbers.Clear()
        for key,value in values do
            let message = JsonObject()
            message["set"] <- JsonValue.Create(key)
            message["value"] <- JsonValue.Create(value)
            send message
    let action name =
        flushNumbers()
        let message = JsonObject()
        message["action"] <- JsonValue.Create(name: string)
        send message
    /// The three source buttons. Rebuilding the capture takes the engine a
    /// second or two, and nothing on screen used to say so - the button simply
    /// sat there looking ignored, so people pressed it again. The spinner the
    /// rest of the section already uses answers immediately instead.
    let sourceAction name =
        preparing <- true
        preparingSince <- DateTime.UtcNow
        this.RaisePropertyChanged("IsPreparing")
        action name

    /// The bars. A click on a switch deserves its tick; a drag along a bar
    /// produced a stream of them, and the timer that sends the final value
    /// ticked once more after the hand had already let go.
    let sliderKeys =
        Set.ofList [ "passes"; "work_scale"; "intensity"; "local_tone"; "local_structure"; "skin_structure"; "split"; "halo"
                     "sharpness"; "edge_smooth"; "brightness"; "contrast"; "saturation"; "warmth"; "frame_limit"
                     "keep_colors"; "low_light" ]

    /// The frame limit's stops, in frames a second shown (0 = off): the usual
    /// refresh rates and the halves of them. The worker takes any 20..360, so
    /// a value saved by hand lands on the nearest stop here.
    let frameLimits = [| 0; 30; 40; 45; 48; 50; 60; 72; 75; 90; 100; 120; 144; 165; 170; 180; 200; 240; 360 |]

    let set key (value: JsonNode) =
        if ready && not updating then
            if key = "nr" || key = "nr_small" || key = "window" || key = "passes" || key = "work_scale" || key = "gpu" || key = "monitor" then
                preparing <- true
                preparingSince <- DateTime.UtcNow
                this.RaisePropertyChanged("IsPreparing")
            let message = JsonObject()
            message["set"] <- JsonValue.Create(key: string)
            message["value"] <- value
            send message
            if not (sliderKeys.Contains key) then sound ()
    let setBool (key: string) (value: bool) = if flag key false <> value then set key (JsonValue.Create(value))
    // SMFG (round 33): LIVE FLOW's switches it shares, whether DLSS 5 is on (its frames are handed over then), and
    // the hand-over itself ("mfg_link", the engine's lf_msm_link.inl).
    let smfgMonitor () =
        let digits = str "monitor_devicename" "" |> Seq.filter Char.IsDigit |> Seq.toArray |> String
        match Int32.TryParse digits with
        | true, n -> n
        | _ -> 0
    /// The window picked with "Select window" (its handle from the "<hex>: title" LIVE FLOW reports), 0 the whole screen.
    let smfgWindow () =
        if windowPick && flag "window_mode" false then
            let current = str "window_current" ""
            match current.IndexOf(':') with
            | at when at > 0 ->
                match Int64.TryParse(current.Substring(0, at), Globalization.NumberStyles.HexNumber, Globalization.CultureInfo.InvariantCulture) with
                | true, handle -> handle
                | _ -> 0L
            | _ -> 0L
        else 0L
    let smfg =
        new SmfgViewModel(
            (fun () ->
                { Vrr = flag "vrr" false; FrameLimit = int (number "frame_limit" 0.); Hdr = flag "hdr" false
                  Spout = flag "spout" false; Counter = counterOn && not counterHeld
                  CounterX = fst counterOffset; CounterY = snd counterOffset
                  Monitor = smfgMonitor (); Link = flag "nr" false; Window = smfgWindow () }),
            (fun () -> ready && not stopping),
            (fun on -> setBool "mfg_link" on),
            (fun () -> str "mfg_link_state" "off"),
            (fun () -> not (usesFlow ())))
    // The FPS counter shows SMFG's figures while it runs (the engine draws the same counter in its frames).
    do smfg.PropertyChanged.Add(fun _ -> if counterOn then this.RaiseCounter())
    let mutable smfgLooked = false
    /// Which bound property each engine key actually feeds.
    ///
    /// Dragging a slider used to end in `notify ()`, which raises a change on
    /// *every* property at once. That re-evaluated every binding on the page -
    /// the seven search predicates included, and each of those splits and
    /// lower-cases strings on every pass. At one tick per 0.01 that is a
    /// hundred full-page invalidations per sweep, which is what made the
    /// comparison wipe crawl. Naming the property costs nothing instead.
    let numberProperties =
        dict [ "passes", [| "PassCount"; "PassIndex"; "PassCountLabel" |]
               "work_scale", [| "WorkScale"; "WorkSize" |]
               "intensity", [| "Intensity" |]
               "local_tone", [| "LocalTone" |]
               "local_structure", [| "LocalStructure" |]
               "skin_structure", [| "SkinStructure" |]
               "split", [| "Split" |]
               "halo", [| "Denoiser"; "DenoiserLabel" |]
               "sharpness", [| "Sharpness"; "SharpnessLabel" |]
               "edge_smooth", [| "EdgeSmoothing"; "EdgeSmoothingLabel" |]
               "keep_colors", [| "KeepColors"; "KeepColorsLabel" |]
               "low_light", [| "LowLight"; "LowLightLabel" |]
               "nr_model", [| "NrModelIndex" |]
               "brightness", [| "Brightness"; "BrightnessLabel"; "HasColors" |]
               "contrast", [| "Contrast"; "ContrastLabel"; "HasColors" |]
               "saturation", [| "Saturation"; "SaturationLabel"; "HasColors" |]
               "warmth", [| "Warmth"; "WarmthLabel"; "HasColors" |]
               "frame_multiplier", [| "FrameMultiplierIndex" |]
               "flow_multiplier", [| "FlowMultiplierIndex" |]
               "frame_limit", [| "FrameLimitIndex"; "FrameLimitLabel" |]
               "style", [| "ModelIndex" |] ]

    /// Falls back to the blanket refresh for a key nobody has mapped, so a new
    /// slider still updates correctly before anyone remembers this table.
    let raiseFor (key: string) =
        match numberProperties.TryGetValue(key) with
        | true, names -> for name in names do this.RaisePropertyChanged(name)
        | _ -> notify ()

    let sendSplit () =
        splitTimer.Stop()
        if not (Double.IsNaN splitPending) then
            let message = JsonObject()
            message["set"] <- JsonValue.Create("split")
            message["value"] <- JsonValue.Create(splitPending)
            send message
            lastSplitSent <- DateTime.UtcNow
            splitPending <- Double.NaN

    let setNumber (key: string) (value: float) =
        if ready && not updating && (abs(number key Double.NaN - value) > 0.0001 || state[key] = null) then
            state[key] <- JsonValue.Create(value)
            recentEdits[key] <- (value, DateTime.UtcNow)
            if key = "split" then
                splitPending <- value
                if (DateTime.UtcNow - lastSplitSent).TotalMilliseconds >= 33. then sendSplit ()
                elif not splitTimer.IsEnabled then splitTimer.Start()
            else
                pendingNumbers[key] <- value
                numberTimer.Stop()
                numberTimer.Start()
            if not (sliderKeys.Contains key) then sound ()
            raiseFor key
    let setString (key: string) (value: string) = if not (String.IsNullOrWhiteSpace(value)) && str key "" <> value then set key (JsonValue.Create(value))

    let findNative () =
        let rec search (dir: DirectoryInfo) =
            if isNull dir then failwith "Screen runtime folder was not found."
            let candidate = Path.Combine(dir.FullName, "mod files", "native")
            if File.Exists(Path.Combine(candidate, "screen", "controller.py")) then candidate
            else search dir.Parent
        search (DirectoryInfo(AppContext.BaseDirectory))

    let receive (line: string) =
        try
            let message = JsonNode.Parse(line)
            if message["state"] <> null then
                let next = message["state"].AsObject()
                // Flatten the upstream parameter group only for the bindings.
                match next["params"] with
                | :? JsonObject as parameters ->
                    for pair in parameters do next[pair.Key] <- pair.Value.DeepClone()
                | _ -> ()
                Dispatcher.UIThread.Post(fun () ->
                    // The FPS counter reads its figures first; they are taken out
                    // below with the other counters.
                    this.TakeCounter(next)
                    // Frame counters are diagnostic data; they must not redraw
                    // every control while the user's settings remain unchanged.
                    next.Remove("frames") |> ignore
                    next.Remove("display_fps") |> ignore
                    next.Remove("live_fps") |> ignore
                    next.Remove("counter_area") |> ignore
                    for pair in pendingNumbers do next[pair.Key] <- JsonValue.Create(pair.Value)
                    if not (Double.IsNaN splitPending) then next["split"] <- JsonValue.Create(splitPending)
                    for pair in recentEdits |> Seq.toArray do
                        let (value, at) = pair.Value
                        if (DateTime.UtcNow - at).TotalSeconds < 1.5 then next[pair.Key] <- JsonValue.Create(value)
                        else recentEdits.Remove(pair.Key) |> ignore
                    let changed = not ready || not (JsonNode.DeepEquals(state, next))
                    updating <- true
                    try
                        state <- next
                        if preparing && (DateTime.UtcNow-preparingSince).TotalSeconds > 0.8 then
                            preparing <- false
                            this.RaisePropertyChanged("IsPreparing")
                        let selected = if flag "preset_active" false then str "profile" "" else ""
                        if selected <> selectedPreset then
                            selectedPreset <- selected
                            if selected <> "" then presetName <- selected
                        ready <- not stopping
                        status <-
                            if stopping then "Stopping Screen…"
                            elif flag "worker_failed" false then "Processing stopped — the worker failed."
                            elif state["gpu_ok"] <> null && not (flag "gpu_ok" true) then "The selected GPU could not start neural processing."
                            elif not (flag "nr" true) then "Ready"
                            elif flag "idle" false then "Ready — waiting for the screen to change"
                            else "Processing"
                        error <- str "error" ""
                        if changed then notify ()
                        if ready && not smfgLooked then
                            smfgLooked <- true
                            smfg.CheckEngine()
                            smfg.LoadCards()
                        // Round 42: LIVE FLOW's worker started again (passes, network model, screen, a recovery).
                        let worker = int (number "worker_pid" 0.)
                        if worker <> 0 then
                            if workerSeen <> 0 && worker <> workerSeen then smfg.LiveFlowRestarted()
                            workerSeen <- worker
                        smfg.LiveFlowChanged()
                    finally updating <- false
                    // LIVE FLOW makes no frames in between any more (round 32:
                    // MFG SUPER MOTION does, linked): DLSS MFG or FLOW MFG left
                    // on by an older version would run with nothing to turn it off.
                    if ready && (flag "flow_mfg" false || flag "frame_generation" false)
                       && (DateTime.UtcNow - flowOffAsked).TotalSeconds > 5. then
                        flowOffAsked <- DateTime.UtcNow
                        for key in [ "flow_mfg"; "frame_generation" ] do
                            if flag key false then
                                let message = JsonObject()
                                message["set"] <- JsonValue.Create(key)
                                message["value"] <- JsonValue.Create(false)
                                send message)
            elif message["error"] <> null then
                let text = message["error"].GetValue<string>()
                Dispatcher.UIThread.Post(fun () -> setError text)
        with _ -> () // Only our JSON status records belong to the control channel.

    do
        try
            if File.Exists(captureConfig) then
                let cfg = JsonNode.Parse(File.ReadAllText(captureConfig))
                outputFolder <- cfg["folder"].GetValue<string>()
                microphone <- cfg["microphone"].GetValue<string>()
                systemAudio <- cfg["system_audio"].GetValue<bool>()
                recordingFps <- cfg["fps"].GetValue<int>()
        with _ -> ()
        try
            if File.Exists(hotkeysConfig) then
                let cfg = JsonNode.Parse(File.ReadAllText(hotkeysConfig))
                overlayMods <- cfg["overlay_mods"].GetValue<uint32>()
                overlayKey <- cfg["overlay_key"].GetValue<uint32>()
                dlssMods <- cfg["dlss_mods"].GetValue<uint32>()
                dlssKey <- cfg["dlss_key"].GetValue<uint32>()
                // Files from before round 30 have no screenshot shortcut: F10.
                if cfg["shot_key"] <> null && cfg["shot_mods"] <> null then
                    shotMods <- cfg["shot_mods"].GetValue<uint32>()
                    shotKey <- cfg["shot_key"].GetValue<uint32>()
                // Files from before round 34 have no recording shortcut: Shift+F9.
                if cfg["rec_key"] <> null && cfg["rec_mods"] <> null then
                    recMods <- cfg["rec_mods"].GetValue<uint32>()
                    recKey <- cfg["rec_key"].GetValue<uint32>()
        with _ -> ()
        try
            if File.Exists(sourceConfig) then
                let cfg = JsonNode.Parse(File.ReadAllText(sourceConfig))
                if cfg["pick"] <> null then windowPick <- cfg["pick"].GetValue<bool>()
        with _ -> ()
        try
            if File.Exists(counterConfig) then
                let cfg = JsonNode.Parse(File.ReadAllText(counterConfig))
                counterOn <- cfg["on"].GetValue<bool>()
                // Older files have no position: the corner it always had.
                if cfg["dx"] <> null && cfg["dy"] <> null then
                    counterOffset <- (cfg["dx"].GetValue<int>(), cfg["dy"].GetValue<int>())
                // A "hold" left behind by a session that ended with the overlay open.
                if cfg["hold"] <> null && cfg["hold"].GetValue<bool>() then saveCounter ()
        with _ -> ()
        try
            if File.Exists(sectionsConfig) then
                match JsonNode.Parse(File.ReadAllText(sectionsConfig))["open"] with
                | :? JsonArray as keys ->
                    for key in keys do
                        let name = key.GetValue<string>()
                        if sectionNames |> List.exists (fun (k, _) -> k = name) then openSections.Add(name) |> ignore
                | _ -> ()
        with _ -> ()
        numberTimer.Tick.Add(fun _ ->
            numberTimer.Stop()
            let values = pendingNumbers |> Seq.map (fun p -> p.Key,p.Value) |> Seq.toArray
            pendingNumbers.Clear()
            for key,value in values do set key (JsonValue.Create(value)))
        splitTimer.Tick.Add(fun _ -> sendSplit ())

    /// The translations, read from the one set the whole app shares.
    ///
    /// This section has its own view-model, and the overlay puts it inside a
    /// window of its own - so neither can reach MainViewModel, which is where
    /// the language actually lives.
    member _.Loc = Localization.current

    /// The language changed underneath us. MainViewModel calls this because it
    /// is the only thing that knows.
    member this.RaiseLoc() =
        this.RaisePropertyChanged("Loc")
        this.RaisePropertyChanged("FpsButtonText")
        this.RaisePropertyChanged("CounterBaseLabel")
        for name in [ "FgQueues"; "FgQueueIndex"; "MfgOptions"; "DlssMfgIndex"; "FlowMfgIndex"; "ColorSpaces"
                      "ColorSpaceIndex"; "SelectSourceText"; "FrameLimitLabel"; "PassCountLabel"; "DenoiserLabel"
                      "SharpnessLabel"; "EdgeSmoothingLabel"; "BrightnessLabel"; "ContrastLabel"; "SaturationLabel"
                      "WarmthLabel"; "KeepColorsLabel"; "LowLightLabel"; "TitleEffect"; "TitleMfg"; "TitleSmfg"; "TitleFlow"; "TitleColors"; "TitleComparison"
                      "TitlePresets"; "TitleSettings"; "TitleRecord"; "ScreenshotButtonText"; "TitleOverlay" ] do
            this.RaisePropertyChanged(name)

    // ---- FPS counter ------------------------------------------------------
    member _.IsFpsCounterOn = counterOn

    member this.ToggleFpsCounter() =
        counterOn <- not counterOn
        saveCounter ()
        smfg.LiveFlowChanged()
        this.RaiseCounter()

    /// The overlay opened or closed: open, the manager's movable counter
    /// stands in for the one the worker draws.
    member this.HoldCounter(held: bool) =
        if held <> counterHeld then
            counterHeld <- held
            saveCounter ()
            smfg.LiveFlowChanged()
            this.RaiseCounter()

    /// The button beside the two shortcuts, and the same one in the overlay.
    member _.FpsButtonText =
        if counterOn then Localization.current.LfFpsOn else Localization.current.LfFpsOff

    /// On, with a session that is really sending frames - and not a stale
    /// figure from one that has gone quiet.
    member _.IsCounterShown =
        counterOn && counterHeld && ready && child.IsSome
        && (smfg.CounterBase >= 0 || (not (Double.IsNaN liveFps) && (DateTime.UtcNow - counterAt).TotalSeconds < 2.5))

    /// The movable counter's text, as the worker and SMFG's engine draw it (MFG SUPER MOTION's look):
    /// "95", or "95/380" while frames in between are made.
    member this.CounterText =
        if smfg.CounterBase >= 0 then
            if smfg.CounterShown >= 0 then sprintf "%d/%d" smfg.CounterBase smfg.CounterShown else string smfg.CounterBase
        elif this.HasCounterOut then this.CounterBaseText + "/" + this.CounterOutText
        else this.CounterBaseText

    member _.CounterBaseText = if Double.IsNaN liveFps then "-" else sprintf "%.0f" liveFps

    /// Frames after the selected generator, measured by its presenter.
    member _.HasCounterOut = counterMode <> "" && not (Double.IsNaN displayFps) && displayFps >= 0.0
    member _.CounterOutText = if Double.IsNaN displayFps then "-" else sprintf "%.0f" displayFps
    member _.CounterModeText = counterMode

    member this.CounterBaseLabel =
        if this.HasCounterOut then Localization.current.LfFpsBase else "FPS"

    /// The output screen, desktop pixels: x, y, width, height.
    member _.CounterArea = counterArea

    /// From the output screen's corner. Kept inside the screen, so a monitor
    /// that has since got smaller cannot lose the counter off its edge.
    member _.CounterOffset =
        let (_, _, w, h) = counterArea
        let (dx, dy) = counterOffset
        if w > 0 && h > 0 then (max 0 (min (w - 40) dx), max 0 (min (h - 30) dy)) else (dx, dy)

    /// The counter was dropped somewhere new (dragged while the overlay is open).
    member _.MoveCounter(dx: int, dy: int) =
        if counterOffset <> (dx, dy) then
            counterOffset <- (dx, dy)
            saveCounter ()
            smfg.LiveFlowChanged()

    member private this.RaiseCounter() =
        for name in [ "IsFpsCounterOn"; "FpsButtonText"; "IsCounterShown"; "CounterBaseText"; "HasCounterOut"
                      "CounterOutText"; "CounterModeText"; "CounterBaseLabel"; "CounterArea"; "CounterText" ] do
            this.RaisePropertyChanged(name)

    /// Reads the counter's figures out of a state record.
    member private this.TakeCounter(next: JsonObject) =
        let num (key: string) =
            match next[key] with
            | null -> Double.NaN
            | v -> try v.GetValue<float>() with _ -> Double.NaN

        let on (key: string) =
            match next[key] with
            | null -> false
            | v -> try v.GetValue<bool>() with _ -> false

        liveFps <- num "live_fps"
        // Idle (an unchanged screen): nothing is being multiplied either.
        displayFps <- if on "idle" && not (on "frame_generation" || on "flow_mfg") then Double.NaN else num "display_fps"

        let step (key: string) =
            let running = num "running_multiplier"
            let chosen = num key
            let n = if not (Double.IsNaN running) && running >= 2.0 then running else chosen
            if Double.IsNaN n then "" else sprintf " ×%.0f" n

        counterMode <-
            if on "frame_generation" then "DLSS FG" + step "frame_multiplier" + " max"
            elif on "flow_mfg" then "FLOW MFG" + step "flow_multiplier"
            else ""

        match next["counter_area"] with
        | :? JsonArray as a when a.Count = 4 ->
            try
                counterArea <- (a[0].GetValue<int>(), a[1].GetValue<int>(), a[2].GetValue<int>(), a[3].GetValue<int>())
            with _ -> ()
        | _ -> ()

        counterAt <- DateTime.UtcNow
        if counterOn then this.RaiseCounter()

    /// The view's timer asks this every half second, so a session that stops
    /// sending takes the counter down with it.
    member this.RefreshCounterShown() = this.RaisePropertyChanged("IsCounterShown")

    // ---- shortcuts -------------------------------------------------------
    member _.OverlayHotkey = (overlayMods, overlayKey)
    member _.DlssHotkey = (dlssMods, dlssKey)
    member _.ScreenshotHotkey = (shotMods, shotKey)
    member _.RecordHotkey = (recMods, recKey)
    member _.RecordKeyText = keyText recMods recKey
    /// The Record shortcut's button among the shortcuts, its key on its face.
    member _.RecordButtonText = Localization.current.LfRecord + " · " + keyText recMods recKey
    member _.RecordCapText =
        if listening = "record" then Localization.current.LfShortcutListening
        else keyText recMods recKey
    member _.ScreenshotKeyText = keyText shotMods shotKey
    /// The Screenshot button - in the Record card and among the shortcuts -
    /// with its key on its face.
    member _.ScreenshotButtonText = Localization.current.LfScreenshot + " · " + keyText shotMods shotKey
    member _.ScreenshotCapText =
        if listening = "shot" then Localization.current.LfShortcutListening
        else keyText shotMods shotKey
    member _.OverlayKeyText = keyText overlayMods overlayKey
    member _.DlssKeyText = keyText dlssMods dlssKey
    /// Whether there is a session to leave. The section starts one as soon as
    /// it is opened; until now the only way out was to leave the page, which
    /// is not something a person guesses (user: give me a way out of the
    /// ready state from the top).
    member _.CanStop = child.IsSome && not stopping
    member _.OverlayButtonText = "Overlay · " + keyText overlayMods overlayKey
    member _.DlssButtonText = "DLSS 5 · " + keyText dlssMods dlssKey

    /// Which shortcut's key cap is waiting for keys, "" when none is.
    member _.Listening = listening

    member _.OverlayCapText =
        if listening = "overlay" then Localization.current.LfShortcutListening
        else keyText overlayMods overlayKey

    member _.DlssCapText =
        if listening = "dlss" then Localization.current.LfShortcutListening
        else keyText dlssMods dlssKey

    [<CLIEvent>]
    member _.HotkeysChanged = hotkeysChanged.Publish

    member private this.RaiseShortcuts() =
        for name in [ "OverlayKeyText"; "DlssKeyText"; "OverlayButtonText"; "DlssButtonText"; "OverlayCapText"; "DlssCapText"; "Listening"
                      "ScreenshotKeyText"; "ScreenshotButtonText"; "ScreenshotCapText"
                      "RecordKeyText"; "RecordButtonText"; "RecordCapText"; "RecordLabel" ] do
            this.RaisePropertyChanged(name)

    /// A second click on the same cap puts it back rather than leaving it
    /// waiting for keys nobody means to press.
    member this.StartListening(target: string) =
        listening <- (if listening = target then "" else target)
        this.RaiseShortcuts()

    member this.StopListening() =
        if listening <> "" then
            listening <- ""
            this.RaiseShortcuts()

    member this.SetHotkey(target: string, mods: uint32, vk: uint32) =
        // Against the other two, whichever this one is.
        let others =
            [ if target <> "overlay" then yield (overlayMods, overlayKey)
              if target <> "dlss" then yield (dlssMods, dlssKey)
              if target <> "shot" then yield (shotMods, shotKey)
              if target <> "record" then yield (recMods, recKey) ]

        let clash = others |> List.contains (mods, vk)

        // A bare letter or digit registered system-wide would stop typing that
        // character in every other program. Function keys are fine alone.
        let isFunctionKey = vk >= 0x70u && vk <= 0x87u

        if clash then
            setError "That combination is already the other shortcut."
        elif mods = 0u && not isFunctionKey then
            setError "Add Ctrl, Alt or Shift - a single letter as a global shortcut would stop working in every other program."
        else
            if target = "overlay" then
                overlayMods <- mods
                overlayKey <- vk
            elif target = "shot" then
                shotMods <- mods
                shotKey <- vk
            elif target = "record" then
                recMods <- mods
                recKey <- vk
            else
                dlssMods <- mods
                dlssKey <- vk

            setError ""
            saveHotkeys ()
            this.RaiseShortcuts()
            hotkeysChanged.Trigger()

    /// The DLSS 5 shortcut: on if it was off, off if it was on.
    member this.ToggleNr() = this.Nr <- not this.Nr

    member _.IsRunning = child.IsSome
    member _.IsPreparing = child.IsSome && not stopping && (preparing || not ready || (flag "nr" false && state["gpu_ok"] = null))
    member _.PassCount with get () = float(this.PassIndex + 1) and set (v: float) = setNumber "passes" (Math.Clamp(Math.Round(v), 1., 6.))
    /// Every slider in the card carries its own value in its own label, and
    /// nothing sits between two columns to be read as the wrong slider's
    /// number (user: "there is a 1x in front of halo reduction"). It also
    /// leaves all four rows the same shape - label, bar - so the two columns
    /// line up exactly, two by two.
    member this.PassCountLabel = sprintf "%s  %g×" Localization.current.LfMultiPass this.PassCount
    member this.DenoiserLabel = sprintf "%s  %.0f%%" Localization.current.LfDenoiser (this.Denoiser * 100.)
    member this.SharpnessLabel = sprintf "%s  %.0f%%" Localization.current.LfSharpness (this.Sharpness * 100.)
    member this.EdgeSmoothingLabel = sprintf "%s  %.0f%%" Localization.current.LfEdgeSmoothing (this.EdgeSmoothing * 100.)
    member this.BrightnessLabel = sprintf "%s  %s" Localization.current.LfBrightness (ScreenViewModel.Signed this.Brightness)
    member this.ContrastLabel = sprintf "%s  %s" Localization.current.LfContrast (ScreenViewModel.Signed this.Contrast)
    member this.SaturationLabel = sprintf "%s  %s" Localization.current.LfSaturation (ScreenViewModel.Signed this.Saturation)
    member this.WarmthLabel = sprintf "%s  %s" Localization.current.LfWarmth (ScreenViewModel.Signed this.Warmth)

    /// -1..1 as the card shows it: "+25", "-40", and a plain "0" at rest.
    static member Signed(value: float) =
        let n = Math.Round(value * 100.)
        if n = 0. then "0" elif n > 0. then sprintf "+%.0f" n else sprintf "%.0f" n
    member _.IsRecording = flag "recording" false
    member _.CanConfigureCapture = not (flag "recording" false)
    member _.CaptureMessage = str "capture_message" ""
    member _.RecordingTime = sprintf "%02.0f:%02.0f" (floor(number "recording_seconds" 0. / 60.)) (number "recording_seconds" 0. % 60.)
    member _.Microphones = let values = items "microphones" in if values.Length = 0 then noMicrophones else values
    member _.Microphone
        with get () = microphone
        and set v =
            if not updating && not (isNull v) && v <> microphone then
                microphone <- v
                saveCapture()
                this.RaisePropertyChanged("Microphone")
    member _.SystemAudio
        with get () = systemAudio
        and set v =
            if not updating && v <> systemAudio then
                systemAudio <- v
                saveCapture()
                this.RaisePropertyChanged("SystemAudio")
    member _.RecordingRates = recordingRates
    member _.RecordingRateIndex
        with get () = if recordingFps = 60 then 0 else 1
        and set v =
            let fps = if v = 1 then 30 else 60
            if not updating && v >= 0 && v <= 1 && recordingFps <> fps then
                recordingFps <- fps
                saveCapture()
                this.RaisePropertyChanged("RecordingRateIndex")
    member _.OutputFolder
        with get () = outputFolder
        and set v =
            if not updating && not (String.IsNullOrWhiteSpace(v)) && outputFolder <> v then
                outputFolder <- v
                saveCapture()
                this.RaisePropertyChanged("OutputFolder")
    member _.ReportError(text: string) = setError text
    member _.Capture(name: string) =
        flushNumbers()
        let message = JsonObject()
        message["action"] <- JsonValue.Create(name)
        message["folder"] <- JsonValue.Create(outputFolder)
        message["microphone"] <- JsonValue.Create(microphone)
        message["system_audio"] <- JsonValue.Create(systemAudio)
        message["fps"] <- JsonValue.Create(recordingFps)
        send message
    member _.ScreenshotCommand = RelayCommand(fun () -> this.Capture("screenshot"))
    /// The screenshot shortcut: the same as the button, when there is a
    /// session to take it from.
    member this.TakeScreenshot() = if ready && child.IsSome then this.Capture("screenshot")
    member _.RecordCommand = RelayCommand(fun () -> this.Capture(if this.IsRecording then "stop_recording" else "start_recording"))
    /// The recording shortcut: starts recording, or stops the one running.
    member this.ToggleRecording() =
        if ready && child.IsSome then this.Capture(if this.IsRecording then "stop_recording" else "start_recording")
    member _.RecordLabel = if this.IsRecording then "Stop recording" else "Record · " + keyText recMods recKey
    member _.RefreshSourcesCommand = RelayCommand(fun () -> sourceAction "refresh_sources")
    member _.FgResolutions = fgResolutions
    member _.FgResolutionIndex
        with get () = match int (number "frame_generation_height" 1080.) with 0 -> 0 | 720 -> 2 | _ -> 1
        and set value = if value >= 0 && value <= 2 then set "frame_generation_height" (JsonValue.Create([| 0; 1080; 720 |][value]))
    /// DLSS MFG's Queue target (round 28, lf_fg_pace.inl): 0 least delay,
    /// 1 balanced, 2 smoothest - as Lossless Scaling names it.
    member _.FgQueues =
        let loc = Localization.current
        [| loc.LfFgQueue0; loc.LfFgQueue1; loc.LfFgQueue2 |]
    member _.FgQueueIndex
        with get () = max 0 (min 2 (int (number "frame_generation_queue" 1.)))
        and set value = if value >= 0 && value <= 2 then set "frame_generation_queue" (JsonValue.Create(value))
    /// The button at the top (round 29), the way Lossless Scaling does it:
    /// press, click the game, and when the count runs out the window in front
    /// is taken. While it counts the same button calls it off.
    member _.SelectSourceText =
        let seconds = int (number "source_countdown" 0.)
        if seconds > 0 then sprintf "%s · %d s" Localization.current.LfSelectCancel seconds
        else sprintf "%s · %d s" Localization.current.LfSelectWindow sourceCount
    /// What is taken now, beside the button: the window's title without the
    /// handle in front of it. Nothing while the whole screen is the source.
    member _.SourceText =
        if flag "window_mode" false then
            let current = str "window_current" ""
            match current.IndexOf(": ") with
            | at when at >= 0 && at < 12 -> current.Substring(at + 2)
            | _ -> current
        else ""
    member this.HasSourceText = this.SourceText <> ""
    /// "Select window" (round 37): the button at the top, there only while this is on.
    member _.IsWindowPickOn = windowPick
    member this.SetWindowPick(on: bool) =
        if on <> windowPick then
            windowPick <- on
            saveSource ()
            // Off: whatever count runs is called off, and a window taken goes back to the whole screen.
            if not on then
                if number "source_countdown" 0. > 0. then
                    let message = JsonObject()
                    message["action"] <- JsonValue.Create("cancel_source")
                    send message
                if flag "window_mode" false then sourceAction "window_mode"
            this.RaisePropertyChanged("IsWindowPickOn")
            smfg.LiveFlowChanged()
    member this.WindowPickOnCommand = RelayCommand(fun () -> this.SetWindowPick true)
    member this.WindowPickOffCommand = RelayCommand(fun () -> this.SetWindowPick false)
    member _.SelectActiveSourceCommand = RelayCommand(fun () ->
        flushNumbers()
        let message = JsonObject()
        message["action"] <- JsonValue.Create(if number "source_countdown" 0. > 0. then "cancel_source" else "start_source")
        message["delay"] <- JsonValue.Create(sourceCount)
        send message)
    member _.RetrySourceCommand = RelayCommand(fun () -> sourceAction "retry_source")
    member _.FocusSourceCommand = RelayCommand(fun () -> try ScreenDesktop.focus this.Window with ex -> setError ex.Message)
    member _.FastResolutionCommand = RelayCommand(fun () -> this.WorkScale <- min this.ScaleCap 0.5)
    member _.BalancedResolutionCommand = RelayCommand(fun () -> this.WorkScale <- min this.ScaleCap 0.65)
    member _.QualityResolutionCommand = RelayCommand(fun () -> this.WorkScale <- min this.ScaleCap 0.85)
    member _.ApplyQuery(value: string) =
        query <- if isNull value then "" else value.Trim()
        notify ()
    member _.ShowProcessing = matches "processing dlss 5 nr network resolution multipass multi-pass passes denoiser denoise noise grain halo outline flow super resolution upscaler upscale scaling lossless sharpness sharpen edge smoothing anti-aliasing fast balanced quality معالجة دقة تمريرات ميلتي باس تشويش حبيبات هالة تكبير رفع الدقة حدة تنعيم حواف"
    /// DLSS MFG for RTX cards; in AMD MODE its place is FLOW MFG's.
    /// Frame generation is MFG SUPER MOTION's (round 32): the two cards of
    /// LIVE FLOW's own generators are off the page, on every card.
    member _.ShowSmfg = matches "multi frame generation smfg mfg super motion generator multiplier mode x2 x3 x4 x5 x6 dlss mfg xess game type flow upscaler sharpness queue target low latency sync vsync motion estimation still pixels guard gpu priority capture dxgi wgc توليد اطارات إطارات مضاعفة"
    member _.ShowDlssMfg = false && matches "dlss mfg fg frame generation multiplier x2 x3 x4 queue target resolution low latency reflex anti-lag input lag latency rtx توليد اطارات إطارات مضاعفة طابور تأخير زمن الاستجابة"
    member _.ShowFlowMfg = false && matches "flow mfg frame generation multiplier x2 x3 x4 performance quality flow view low latency anti-lag input lag latency amd radeon توليد اطارات إطارات مضاعفة تأخير زمن الاستجابة"
    member _.ShowFps = matches "fps vrr g-sync gsync freesync variable refresh frame limit fps cap limiter rtss counter حد الاطارات الإطارات معدل التحديث المتغير عداد"
    member _.ShowEffect = matches "effect profile model default natural cinematic intensity local tone local structure skin structure تأثير بروفايل موديل تون بشرة قوة"
    member _.ShowComparison = matches "comparison before after wipe split مقارنة قبل بعد خط"
    member _.ShowSettings = matches "settings processing gpu display motion estimation cpu nvofa skip static frames إعدادات كرت شاشة حركة"
    member _.ShowCapture = matches "capture screenshot printscreen windows win shift s obs recording spout2 plugin microphone audio output folder fps rate تصوير لقطة تسجيل صوت مايك مجلد حفظ"
    member _.ShowPresets = matches ("presets save new update rename delete apply saved name بريست حفظ اسم تعديل حذف " + String.Join(" ", items "saved_presets"))
    member _.ShowColors = matches "colors colours color colour brightness contrast saturation warmth temperature hdr srgb scrgb linear color space colour space ألوان الوان سطوع تباين تشبع دفء فضاء"
    member _.ShowOverlay = matches "overlay shortcut shortcuts hotkey key dlss 5 toggle أوفرلاي اوفرلاي اختصار اختصارات مفتاح"
    member _.NoSearchResults =
        not (this.ShowProcessing || this.ShowSmfg || this.ShowFps || this.ShowEffect
             || this.ShowComparison || this.ShowColors || this.ShowSettings || this.ShowCapture || this.ShowPresets || this.ShowOverlay)

    // ---- card titles: upper case, like the sections of Settings ------------
    member _.TitleEffect = Localization.current.LfEffect.ToUpperInvariant()
    member _.TitleMfg = Localization.current.LfMfgTitle.ToUpperInvariant()
    member _.TitleSmfg = Localization.current.LfSmfgTitle.ToUpperInvariant()
    member _.TitleFlow = Localization.current.LfFlowMfg.ToUpperInvariant()
    member _.TitleColors = Localization.current.LfColorsHdr.ToUpperInvariant()
    member _.TitleComparison = Localization.current.LfComparison.ToUpperInvariant()
    member _.TitlePresets = Localization.current.LfMyPresets.ToUpperInvariant()
    member _.TitleSettings = Localization.current.LfSettings.ToUpperInvariant()
    member _.TitleRecord = Localization.current.LfRecordSection.ToUpperInvariant()
    member _.TitleOverlay = Localization.current.LfOverlaySection.ToUpperInvariant()

    // ---- folded cards (round 29) ------------------------------------------
    // Open when the person opened it - or while a search is on, so what was
    // searched for is not hidden inside a card that matched.
    member private _.IsOpen(key: string) = openSections.Contains(key) || query <> ""
    member private this.Chevron(key: string) =
        if this.IsOpen(key) then "M 7,14 L 12,9 L 17,14 Z" else "M 7,10 L 12,15 L 17,10 Z"
    member this.IsEffectOpen = this.IsOpen("effect")
    member this.IsColorsOpen = this.IsOpen("colors")
    member this.IsFpsOpen = this.IsOpen("fps")
    member this.IsPresetsOpen = this.IsOpen("presets")
    member this.IsSettingsOpen = this.IsOpen("settings")
    member this.IsCaptureOpen = this.IsOpen("capture")
    member this.IsSmfgOpen = this.IsOpen("smfg")
    member this.SmfgChevron = this.Chevron("smfg")
    member this.IsOverlayOpen = this.IsOpen("overlay")
    member this.EffectChevron = this.Chevron("effect")
    member this.ColorsChevron = this.Chevron("colors")
    member this.FpsChevron = this.Chevron("fps")
    member this.PresetsChevron = this.Chevron("presets")
    member this.SettingsChevron = this.Chevron("settings")
    member this.CaptureChevron = this.Chevron("capture")
    member this.OverlayChevron = this.Chevron("overlay")
    member this.ToggleSection(key: string) =
        match sectionNames |> List.tryFind (fun (k, _) -> k = key) with
        | Some (_, name) ->
            if not (openSections.Remove(key)) then openSections.Add(key) |> ignore
            saveSections ()
            this.RaisePropertyChanged("Is" + name + "Open")
            this.RaisePropertyChanged(name + "Chevron")
        | None -> ()
    member _.IsReady = ready && not stopping
    member _.CanStart = child.IsNone

    // ---- AMD MODE: LIVE FLOW for AMD Radeon (ALPHA) ------------------------
    member _.IsRadeonMode = amdMode

    /// MainViewModel says when AMD MODE is switched in Settings. The two
    /// versions are different workers, so whatever is running stops; the next
    /// start takes the one that matches.
    member this.SetAmdMode(value: bool) =
        if amdMode <> value then
            amdMode <- value
            if child.IsSome then this.StopAsync() |> ignore
            notify ()

    /// Multi-pass and NVIDIA Optical Flow exist on NVIDIA cards only: in the
    /// Radeon version they stay where they are, greyed out, and say why.
    member _.CanMultiPass = not amdMode
    member _.MultiPassTip = if amdMode then Localization.current.LfNvidiaOnly else Localization.current.LfMultiPass
    member _.CanPickMotion = not amdMode
    member _.MotionTip : string = if amdMode then Localization.current.LfRadeonMotion else null
    member _.Status = status
    member _.Error = error
    member _.HasError = not (String.IsNullOrWhiteSpace(error))
    member _.GpuText = str "gpu_text" ""
    member this.HasGpuText = this.GpuText <> ""
    /// The network's size; with Flow Super Resolution on, also the picture's
    /// (a fifth of the way down from the display, flow_upscaler.inl) and the
    /// display it is scaled to - kept apart on purpose.
    member _.WorkSize =
        let work = str "work_size" "—"
        let screen = str "screen_size" ""
        let picture = str "super_res_size" ""
        if flag "upscaler" false && picture <> "" && screen <> "" && picture <> screen then
            sprintf "%s · %s → %s" work picture screen
        else work
    member _.ScreenSize = str "screen_size" "—"
    member _.ScaleCap = number "work_scale_cap" 1.0
    member _.IsWindowMode = flag "window_mode" false
    member _.CanDeletePreset = flag "preset_active" false
    member _.Profiles = items "builtin_profiles"
    member _.SavedPresets = items "saved_presets"
    member _.PresetName
        with get () = presetName
        and set value =
            if presetName <> value then
                presetName <- value
                this.RaisePropertyChanged("PresetName")
    member _.SelectedPreset
        with get () = if selectedPreset = "" then null else selectedPreset
        and set value = if not (String.IsNullOrWhiteSpace(value)) then setString "profile" value
    member _.Gpus = items "gpus"
    member _.Monitors = items "monitors"
    /// "None" first: follow no window, capture the whole screen. That used to
    /// be a Clear button beside the list, which read as "empty this list"
    /// rather than as a choice of what to capture.
    member _.Windows =
        let current = items "windows"

        if windowsWithNone.Length = 0 || not (obj.ReferenceEquals(current, lastWindows)) then
            lastWindows <- current
            windowsWithNone <- Array.append [| Localization.current.LfNone |] current

        windowsWithNone
    member _.Models = models
    /// GPU first: it is the default, and on an RTX card the better answer.
    member _.MotionBackends =
        let l = Localization.current
        if motionLabels.Length = 0 || motionLabelsFor <> l.Code then
            motionLabels <- [| l.LfMotionGpu; l.LfMotionCpu |]
            motionLabelsFor <- l.Code
        motionLabels
    member _.PassOptions = passOptions
    member _.PassIndex with get () = int (number "passes" 1.0) - 1 and set v = if v >= 0 && v <= 5 then setNumber "passes" (float (v + 1))
    member _.FrameInfo = sprintf "Frames: %.0f   •   Output: %s" (number "frames" 0.0) (str "screen_size" "—")
    member this.HasStepDown = this.StepDownInfo <> ""

    /// What is really being multiplied, when it is not what was picked.
    ///
    /// The runtime steps down when it refuses a multiplier - x4 asked for,
    /// x2 given - and the worker says so in its log. Showing the pick and
    /// nothing else describes something that is not happening; this line
    /// appears only when the two differ.
    member _.StepDownInfo =
        let running = int (number "running_multiplier" 0.0)
        let wanted = if flag "frame_generation" false then int (number "frame_multiplier" 2.0) elif flag "flow_mfg" false then int (number "flow_multiplier" 2.0) else 0
        let error = str "frame_generation_error" ""
        if flag "frame_generation" false && error <> "" then "DLSS FG: " + error
        elif running > 0 && wanted > 0 && running <> wanted then
            sprintf "Running ×%d — ×%d was refused by this GPU." running wanted
        else ""

    member _.Nr with get () = ready && flag "nr" false and set v = setBool "nr" v
    /// Flow Super Resolution (the worker's flow_upscaler.inl): the pipeline -
    /// network, Denoiser, FLOW MFG - runs at the size the resolution slider
    /// picks, and only the output is scaled to the real display. A game in a
    /// smaller window is scaled onto its whole screen. It took Enhance's place
    /// - Enhance itself is always on and has no switch.
    member _.Upscaler with get () = flag "upscaler" false and set v = setBool "upscaler" v
    /// Low Latency (the worker's lf_latency.inl): one frame queued at most,
    /// the capture taken just in time, a smaller FLOW MFG cushion.
    member _.LowLatency with get () = flag "low_latency" false and set v = setBool "low_latency" v
    /// The worker's own measurement: how old a shown frame is when it reaches
    /// the screen, from the moment the game's frame reached the desktop.
    member _.LatencyText =
        match state["latency_ms"] with
        | :? JsonObject as o when o["avg"] <> null ->
            try sprintf "%.1f ms\np95 %.1f" (o["avg"].GetValue<float>()) (o["p95"].GetValue<float>()) with _ -> ""
        | _ -> ""
    member this.HasLatency = this.LatencyText <> "" && child.IsSome
    /// VRR: frames wait for the screen, never tear, capped inside its range
    /// (lf_vrr.inl), which also checks whether the screen really follows.
    member _.Vrr with get () = flag "vrr" false and set v = setBool "vrr" v
    /// What that check found: the screen follows the frames, or stays at its
    /// fixed refresh (the driver does not apply VRR to windows).
    member this.IsVrrActive = this.Vrr && str "vrr_state" "" = "active"
    member this.IsVrrInactive = this.Vrr && str "vrr_state" "" = "inactive"
    member _.Hdr with get () = flag "hdr" false and set v = setBool "hdr" v
    member _.SkipStatic with get () = flag "skip_static" false and set v = setBool "skip_static" v
    member _.ExternalOutput with get () = flag "spout" false and set v = setBool "spout" v
    member _.WorkScale with get () = number "work_scale" 0.65 and set v = setNumber "work_scale" v
    /// The Denoiser, 0..1 in steps of 1%: the worker's noise cleaner
    /// (denoise.inl) and, at half the number, its edge-rim removal
    /// (halo_reduction.inl). 5% unless moved; the engine key stays "halo".
    member _.Denoiser with get () = number "halo" 0.05 and set (v: float) = setNumber "halo" (Math.Clamp(Math.Round(v, 2), 0.0, 1.0))
    /// Contrast-adaptive sharpening of the finished picture, 0..1 (0 = none).
    member _.Sharpness with get () = number "sharpness" 0.0 and set v = setNumber "sharpness" (Math.Clamp(v, 0.0, 1.0))
    /// Smoothing along edges - never across them - against stair-steps, 0..1.
    member _.EdgeSmoothing with get () = number "edge_smooth" 0.0 and set v = setNumber "edge_smooth" (Math.Clamp(v, 0.0, 1.0))
    /// Keep colours, 0..1 (round 29): how much of the picture's own colour is
    /// kept where the network's result goes back on it. 1 by default - the
    /// network alone takes colour out of the picture and drifts towards blue.
    member _.KeepColors with get () = number "keep_colors" 0.0 and set (v: float) = setNumber "keep_colors" (Math.Clamp(Math.Round(v, 2), 0.0, 1.0))
    member this.KeepColorsLabel = sprintf "%s  %.0f%%" Localization.current.LfKeepColors (this.KeepColors * 100.)
    /// The network's model (round 39): 0 Quality (NVIDIA's model A, as NeuralScreen runs it), 1 Fast (model B).
    member _.NrModels = nrModelsList
    member _.NrModelIndex
        with get () = if str "nr_model" "quality" = "fast" then 1 else 0
        and set (v: int) = if v = 0 || v = 1 then setString "nr_model" (if v = 1 then "fast" else "quality")
    /// Low-light noise (round 34): 0..1, the worker's kDetailHlsl.
    member _.LowLight with get () = number "low_light" 0.0 and set (v: float) = setNumber "low_light" (Math.Clamp(Math.Round(v, 2), 0.0, 1.0))
    member this.LowLightLabel = sprintf "%s  %.0f%%" Localization.current.LfLowLight (this.LowLight * 100.)
    // ---- COLOURS: -1..1 each, 0 untouched (post_finish.inl) ---------------
    member _.Brightness with get () = number "brightness" 0.0 and set v = setNumber "brightness" (Math.Clamp(v, -1.0, 1.0))
    member _.Contrast with get () = number "contrast" 0.0 and set v = setNumber "contrast" (Math.Clamp(v, -1.0, 1.0))
    member _.Saturation with get () = number "saturation" 0.0 and set v = setNumber "saturation" (Math.Clamp(v, -1.0, 1.0))
    member _.Warmth with get () = number "warmth" 0.0 and set v = setNumber "warmth" (Math.Clamp(v, -1.0, 1.0))
    /// Anything moved - the reset button shows only then.
    member this.HasColors =
        this.Brightness <> 0.0 || this.Contrast <> 0.0 || this.Saturation <> 0.0 || this.Warmth <> 0.0
    member this.ResetColorsCommand =
        RelayCommand(fun () ->
            this.Brightness <- 0.0
            this.Contrast <- 0.0
            this.Saturation <- 0.0
            this.Warmth <- 0.0)
    member _.CanFrameGeneration = not amdMode
    member _.FrameGenerationHint =
        if amdMode then "DLSS Frame Generation requires a compatible NVIDIA GPU. Use FLOW MFG on AMD."
        else "Experimental desktop DLSS FG / MFG. Support and multiplier depend on the NVIDIA GPU, driver and runtime. Enabling this turns FLOW MFG off."
    member _.FrameGeneration with get () = ready && flag "frame_generation" false and set v = if not amdMode || not v then setBool "frame_generation" v
    member _.FrameMultiplierIndex
        with get () = Math.Clamp(int (number "frame_multiplier" 2.0) - 2, 0, 4)
        and set v = if v >= 0 && v <= 4 then setNumber "frame_multiplier" (float (v + 2))
    /// FLOW MFG and native DLSS FG are exclusive in the controller and worker.
    member _.FlowMfg with get () = ready && flag "flow_mfg" false and set v = setBool "flow_mfg" v
    /// FLOW MFG's performance mode (the worker's flow_mfg.inl, round 26): the
    /// frames in between drawn lightly, the estimate smaller, no tracker - a
    /// fraction of the card's time. On by default; off is the full drawing.
    member _.FlowPerformance with get () = flag "flow_performance" true and set v = setBool "flow_performance" v

    /// FLOW VIEW: FLOW MFG draws how it sees the picture (the engine never
    /// saves it - each session starts with it off).
    member _.FlowView with get () = ready && flag "flow_view" false and set v = setBool "flow_view" v
    member this.ToggleFlowView() = this.FlowView <- not this.FlowView

    /// The legend beside the picture: only while there is something to explain.
    member this.IsFlowLegendShown = this.FlowView && this.FlowMfg && child.IsSome
    member _.FlowMultipliers = flowMultipliers
    member private _.TranslatedLists() =
        let l = Localization.current
        if mfgOptions.Length = 0 || listsFor <> l.Code then
            mfgOptions <- Array.append [| l.LfOff |] flowMultipliers
            colorSpaces <- [| l.LfColorSpaceAuto; "sRGB"; "sRGB Linear"; "scRGB" |]
            listsFor <- l.Code
    /// Off, x2 ... x6: the switch and the multiplier in one list (round 29),
    /// the way Lossless Scaling's multiplier is one.
    member this.MfgOptions =
        this.TranslatedLists()
        mfgOptions
    member this.DlssMfgIndex
        with get () = if this.FrameGeneration then this.FrameMultiplierIndex + 1 else 0
        and set (v: int) =
            if v = 0 then this.FrameGeneration <- false
            elif v >= 1 && v <= 5 && not amdMode then
                // The multiplier first, and at once: sent after the switch it
                // would start the generator twice.
                if int (number "frame_multiplier" 2.0) <> v + 1 then set "frame_multiplier" (JsonValue.Create(v + 1))
                this.FrameGeneration <- true
    member this.FlowMfgIndex
        with get () = if this.FlowMfg then Math.Clamp(this.FlowMultiplierIndex, 0, 4) + 1 else 0
        and set (v: int) =
            if v = 0 then this.FlowMfg <- false
            elif v >= 1 && v <= 5 then
                if int (number "flow_multiplier" 2.0) <> v + 1 then set "flow_multiplier" (JsonValue.Create(v + 1))
                this.FlowMfg <- true
    /// How the output is handed to the screen (the worker's lf_hdr_display.h):
    /// auto, sRGB, linear sRGB or scRGB. The worker starts over for it.
    member this.ColorSpaces =
        this.TranslatedLists()
        colorSpaces
    member _.ColorSpaceIndex
        with get () = match str "color_space" "auto" with "srgb" -> 1 | "linear" -> 2 | "scrgb" -> 3 | _ -> 0
        and set (v: int) =
            if v >= 0 && v <= 3 then
                let name = [| "auto"; "srgb"; "linear"; "scrgb" |][v]
                if str "color_space" "auto" <> name then
                    preparing <- true
                    preparingSince <- DateTime.UtcNow
                    this.RaisePropertyChanged("IsPreparing")
                    set "color_space" (JsonValue.Create(name))
    /// The HDR switch decides only while the colour space is left on auto.
    member _.CanHdrSwitch = str "color_space" "auto" = "auto"
    member this.FpsCounterOnCommand = RelayCommand(fun () -> if not this.IsFpsCounterOn then this.ToggleFpsCounter())
    member this.FpsCounterOffCommand = RelayCommand(fun () -> if this.IsFpsCounterOn then this.ToggleFpsCounter())
    member _.FlowMultiplierIndex
        with get () = int (number "flow_multiplier" 2.0) - 2
        and set v = if v >= 0 && v <= 4 then setNumber "flow_multiplier" (float (v + 2))
    /// The frame limit (RTSS-like; with FLOW MFG it also sets how fast the
    /// network runs - the worker's FrameLimitWait). The slider walks the stops.
    member _.FrameLimitIndex
        with get () =
            let fps = number "frame_limit" 0.0
            if fps < 20. then 0.
            else float (frameLimits |> Array.mapi (fun i v -> i, abs (float v - fps)) |> Array.skip 1 |> Array.minBy snd |> fst)
        and set (v: float) = setNumber "frame_limit" (float frameLimits[Math.Clamp(int (Math.Round v), 0, frameLimits.Length - 1)])
    member _.FrameLimitMaximum = float (frameLimits.Length - 1)
    member this.FrameLimitLabel =
        let fps = frameLimits[int this.FrameLimitIndex]
        if fps = 0 then sprintf "%s  %s" Localization.current.LfFrameLimit Localization.current.LfOff
        else sprintf "%s  %d FPS" Localization.current.LfFrameLimit fps
    member _.Intensity with get () = number "intensity" 1.0 and set v = setNumber "intensity" v
    member _.LocalTone with get () = number "local_tone" 0.5 and set v = setNumber "local_tone" v
    member _.LocalStructure with get () = number "local_structure" 1.0 and set v = setNumber "local_structure" v
    member _.SkinStructure with get () = number "skin_structure" -1.0 and set v = setNumber "skin_structure" v
    member _.Split with get () = number "split" 0.0 and set v = setNumber "split" v
    member _.ModelIndex with get () = int (number "style" 1.0) and set v = if v >= 0 then setNumber "style" (float v)
    member _.Profile with get () = str "profile" "Natural" and set v = setString "profile" v
    member _.Gpu with get () = str "gpu" "" and set v = setString "gpu" v
    member _.Monitor with get () = str "monitor" "" and set v = setString "monitor" v
    member _.Window
        with get () = if flag "window_mode" false then str "window_current" "" else Localization.current.LfNone
        and set (v: string) =
            if not (String.IsNullOrWhiteSpace(v)) then
                if v = Localization.current.LfNone then
                    if flag "window_mode" false then sourceAction "window_mode"
                elif str "window_current" "" <> v then
                    set "window" (JsonValue.Create(v))
    /// 0 = the GPU (NVIDIA Optical Flow), 1 = the CPU. The GPU is the default:
    /// the engine starts there unless the CPU was picked by name.
    member _.MotionBackendIndex
        with get () = if amdMode || str "motion_backend" "nvofa" = "cpu" then 1 else 0
        and set (v: int) =
            // The Radeon version has the CPU only; the NVIDIA choice is kept for when AMD MODE goes off.
            if amdMode then ()
            elif v = 0 then setString "motion_backend" "nvofa"
            elif v = 1 then setString "motion_backend" "cpu"
    member _.Start() =
        if child.IsNone then
            try
                let native = findNative ()
                let python = Path.Combine(native, "runtime", "python.exe")
                // The Radeon version needs its own worker; its network files are
                // checked by the worker itself, which says in the log what is missing.
                let required =
                    if amdMode then [python; Path.Combine(native, "radeon", "liveflow_radeon.exe")]
                    else [python; Path.Combine(native, "nvngx.dll")]
                for path in required do
                    if not (File.Exists(path)) then failwith ("Screen requires: " + path)
                Directory.CreateDirectory(dataDir) |> ignore
                let config = Path.Combine(dataDir, "config.json")
                if not (File.Exists(config)) then
                    File.WriteAllText(config, "{\"monitor\":0,\"width\":1920,\"height\":1080,\"fullscreen\":true,\"warmup\":120,\"work_scale\":0.65,\"profile\":\"Natural\",\"intensity\":1.0,\"local_tone\":0.5,\"local_structure\":1.0,\"skin_structure\":-1.0,\"nr_small\":true,\"worker_present\":true,\"motion_on_gpu\":true,\"capture_in_worker\":true,\"open_menu_on_start\":false,\"theme\":\"dark\",\"lang\":\"en\"}")
                let info = ProcessStartInfo(python, UseShellExecute=false, CreateNoWindow=true, WorkingDirectory=dataDir, RedirectStandardInput=true, RedirectStandardOutput=true, RedirectStandardError=true)
                info.StandardInputEncoding <- UTF8Encoding(false)
                info.StandardOutputEncoding <- Encoding.UTF8
                info.ArgumentList.Add(Path.Combine(native, "screen", "controller.py"))
                info.ArgumentList.Add("--config")
                info.ArgumentList.Add(config)
                info.Environment["DLSS_MANAGER_SCREEN_DATA"] <- dataDir
                info.Environment["DLSS_MANAGER_EXE"] <- Environment.ProcessPath
                info.Environment["DLSS_MANAGER_PID"] <- string Environment.ProcessId
                info.Environment["LIVEFLOW_RADEON"] <- if amdMode then "1" else "0"
                let p = new Process(StartInfo=info, EnableRaisingEvents=true)
                p.OutputDataReceived.Add(fun e -> if not (isNull e.Data) then receive e.Data)
                p.ErrorDataReceived.Add(fun e -> if not (String.IsNullOrWhiteSpace(e.Data)) then Dispatcher.UIThread.Post(fun () -> setError e.Data))
                p.Exited.Add(fun _ ->
                    Dispatcher.UIThread.Post(fun () ->
                        if child |> Option.exists (fun current -> obj.ReferenceEquals(current, p)) then
                            let code = p.ExitCode
                            child <- None
                            ready <- false
                            stopping <- false
                            status <- if code = 0 then "Screen stopped — resources unloaded." else "Screen could not continue. Reopen this section to retry."
                            notify ()))
                error <- ""
                status <- "Starting Screen…"
                p.Start() |> ignore
                child <- Some p
                p.BeginOutputReadLine()
                p.BeginErrorReadLine()
                notify ()
            with ex ->
                child <- None
                status <- "Screen could not start."
                setError ex.Message
                notify ()

    member _.StopAsync() : Task =
        smfg.LiveFlowStopped()
        numberTimer.Stop()
        pendingNumbers.Clear()
        splitTimer.Stop()
        splitPending <- Double.NaN
        recentEdits.Clear()
        task {
            match child with
            | None -> ()
            | Some p when stopping -> do! p.WaitForExitAsync()
            | Some p ->
                stopping <- true
                ready <- false
                status <- "Closing Screen…"
                notify ()
                action "quit"
                try
                    p.StandardInput.Close()
                    let! completed = Task.WhenAny(p.WaitForExitAsync(), Task.Delay(20000))
                    if not p.HasExited then
                        p.Kill(true)
                        do! p.WaitForExitAsync()
                        setError "Screen did not respond to shutdown; its process tree was terminated."
                with ex -> setError ex.Message
        }

    /// Leave the ready state: the same shutdown the page does when it closes,
    /// asked for deliberately. The section can be started again from here.
    member this.StopCommand = RelayCommand(fun () -> this.StopAsync() |> ignore)

    /// Back from the ready state. Without it the only way back in was to leave
    /// the section and come back, which nobody guesses (user: after Exit there
    /// is no button to turn it on again).
    member this.StartCommand = RelayCommand(fun () -> this.Start())
    member _.FullscreenCommand = RelayCommand(fun () -> sourceAction "window_mode")
    // ---- SMFG (round 33): MFG SUPER MOTION's frame generation, in LIVE FLOW ----
    member _.Smfg = smfg
    member _.OpenObsPluginCommand = RelayCommand(fun () ->
        try Process.Start(ProcessStartInfo("https://github.com/Off-World-Live/obs-spout2-plugin/releases", UseShellExecute=true)) |> ignore
        with ex -> setError ex.Message)
    member private _.SaveNamedPreset(update: bool) =
        flushNumbers()
        let message = JsonObject()
        message["action"] <- JsonValue.Create(if update then "update_preset" else "create_preset")
        message["name"] <- JsonValue.Create(presetName)
        message["previous"] <- JsonValue.Create(selectedPreset)
        send message
    member _.SavePresetCommand = RelayCommand(fun () -> this.SaveNamedPreset(false))
    member _.UpdatePresetCommand = RelayCommand(fun () -> this.SaveNamedPreset(true))
    member _.ApplyPresetCommand = RelayCommand(fun () -> if selectedPreset <> "" then set "profile" (JsonValue.Create(selectedPreset)))
    member _.DeletePresetCommand = RelayCommand(fun () -> action "delete_preset")

    member this.NrOffCommand = RelayCommand(fun () -> this.Nr <- false)
    member this.NrOnCommand = RelayCommand(fun () -> this.Nr <- true)

    member this.LowLatencyOffCommand = RelayCommand(fun () -> this.LowLatency <- false)
    member this.LowLatencyOnCommand = RelayCommand(fun () -> this.LowLatency <- true)
    member this.FrameGenerationOffCommand = RelayCommand(fun () -> this.FrameGeneration <- false)
    member this.FrameGenerationOnCommand = RelayCommand(fun () -> this.FrameGeneration <- true)
    member this.FlowMfgOffCommand = RelayCommand(fun () -> this.FlowMfg <- false)
    member this.FlowMfgOnCommand = RelayCommand(fun () -> this.FlowMfg <- true)
    member this.FlowQualityCommand = RelayCommand(fun () -> this.FlowPerformance <- false)
    member this.FlowPerformanceCommand = RelayCommand(fun () -> this.FlowPerformance <- true)

    member this.UpscalerOffCommand = RelayCommand(fun () -> this.Upscaler <- false)
    member this.UpscalerOnCommand = RelayCommand(fun () -> this.Upscaler <- true)
    member this.VrrOffCommand = RelayCommand(fun () -> this.Vrr <- false)
    member this.VrrOnCommand = RelayCommand(fun () -> this.Vrr <- true)

    member this.SkipStaticOffCommand = RelayCommand(fun () -> this.SkipStatic <- false)
    member this.SkipStaticOnCommand = RelayCommand(fun () -> this.SkipStatic <- true)

    member this.HdrOffCommand = RelayCommand(fun () -> this.Hdr <- false)
    member this.HdrOnCommand = RelayCommand(fun () -> this.Hdr <- true)

    member this.ExternalOutputOffCommand = RelayCommand(fun () -> this.ExternalOutput <- false)
    member this.ExternalOutputOnCommand = RelayCommand(fun () -> this.ExternalOutput <- true)

namespace DLSS_5_MANAGER.ViewModels

// MULTI FRAME GENERATION on the LIVE FLOW page (round 33): MFG SUPER MOTION's
// frame generation card, as that app has it (its MainViewModel.fs), on the
// whole screen - DLSS MFG (RTX cards), XeSS MFG, SMFG (any card). The
// engine is downloaded once (SmfgStore); until it is, the card offers only
// that. LIVE FLOW's own switches VRR, frame limit, HDR, Spout and the FPS
// counter are SMFG's too (`shared`); DLSS 5 on hands LIVE FLOW's frames over
// (`setLink`), and turning it on or off while SMFG runs starts the engine
// again on the other source.

open System
open System.Threading
open System.Threading.Tasks
open Avalonia.Threading
open CommunityToolkit.Mvvm.Input
open DLSS_5_MANAGER.Services

type SmfgViewModel(shared: unit -> SmfgShared, ready: unit -> bool, setLink: bool -> unit, linkState: unit -> string,
                    dlssAllowed: unit -> bool) as this =
    inherit ViewModelBase()

    let engine = new SmfgEngine()
    let mutable options = SmfgOptions.load ()
    let mutable status = SmfgStatus.idle
    let mutable wanted = false            // the switch: SMFG on
    let mutable running = false           // asked of the engine (it may still be starting)
    let mutable linked = false            // started on LIVE FLOW's frames
    let mutable startedOn = 0L            // the window it was started on (round 37), 0 the whole screen
    let mutable busy = false              // starting or starting again
    let mutable cards: SmfgCard array = [||]
    let mutable installed = SmfgStore.usable ()
    let mutable latest: SmfgRelease option = None
    let mutable checking = false
    let mutable downloading = false
    let mutable progress = 0.0
    let mutable failed = ""
    let mutable telling = false
    let generatorNames = [| "DLSS MFG"; "XeSS MFG"; "SMFG" |]
    // The lists keep one array while their items stay the same: a new array, even with the same items in it,
    // makes a ComboBox drop its selection and close a list that is open.
    let modesList = [| "×2"; "×3"; "×4"; "×5"; "×6" |]
    let gameTypesList = [| "Auto"; "2D"; "2.5D"; "First person"; "Third person"; "Racing"; "Open world"; "Custom" |]
    let upscalersList = [| "Off"; "Sharpness only"; "Integer"; "Nearest Neighbor"; "Anime4K" |]
    let queueTargetsList = [| "0"; "1"; "2" |]
    let syncModesList = [| "Off (allow tearing)"; "Default"; "VSync"; "VSync ½"; "VSync ⅓" |]
    let gpuPrioritiesList = [| "Realtime"; "Normal" |]
    let captureApisList = [| "DXGI"; "WGC" |]
    let gameFpsList = SmfgOptions.gameFpsRates |> Array.map (fun r -> if r = 0 then "Auto" else string r)
    let dlssPresetsList =
        let loc = Localization.current
        [| loc.LfSmfgPresetDriver; loc.LfSmfgPresetPerformance; loc.LfSmfgPresetQuality; loc.LfSmfgPresetBalanced |]
    let kept = Collections.Generic.Dictionary<string, string array>()
    let keep name (next: string array) =
        match kept.TryGetValue name with
        | true, before when before = next -> before
        | _ -> kept[name] <- next; next
    let post (action: unit -> unit) = Dispatcher.UIThread.Post(fun () -> action ())
    let notify (names: string list) =
        let was = telling
        telling <- true
        try names |> List.iter this.RaisePropertyChanged
        finally telling <- was
    let notifyAll () = this.RaisePropertyChanged("")

    let runsOn generator (card: SmfgCard) = match generator with | 0 -> card.Dlss | 1 -> card.Xess | _ -> true
    // DLSS MFG runs on RTX cards only: never offered in AMD MODE or on a card that is not an RTX.
    let generatorIds () =
        let all = [| 0 .. SmfgOptions.generators - 1 |] |> Array.filter (fun g -> g <> 0 || dlssAllowed ())
        if cards.Length = 0 then all
        else
            match cards |> Array.tryFind (fun c -> c.Name = options.Gpu && runsOn options.Generator c) with
            | Some card -> all |> Array.filter (fun g -> runsOn g card)
            | None -> all |> Array.filter (fun g -> cards |> Array.exists (runsOn g))
    let cardNames generator = cards |> Array.filter (runsOn generator) |> Array.map _.Name
    /// The generator that runs: the one chosen, or the first this card has when it cannot.
    let generatorNow () =
        let ids = generatorIds ()
        if Array.contains options.Generator ids then options.Generator elif ids.Length > 0 then ids[0] else 2
    /// What the engine is given (Adaptive Motion Learning is MFG SUPER MOTION's own, not here).
    let engineOptions () = { options with Generator = generatorNow (); AdaptiveLearning = false }
    let mutable dlssWas: bool option = None
    let mutable relinkCheck: DateTime option = None   // round 42: when to look whether the engine took the new link

    let sendTimer = new Timer((fun _ -> post (fun () -> if running then engine.Send(engineOptions (), { shared () with Link = linked }))), null, Timeout.Infinite, Timeout.Infinite)
    let update change =
        let next = change options |> SmfgOptions.normalize
        if next <> options then
            // The DLSS MFG preset goes into the driver's profile of the engine as it starts.
            let generatorMoved = next.Generator <> options.Generator || next.Gpu <> options.Gpu || next.DlssPreset <> options.DlssPreset
            options <- next
            SmfgOptions.save options
            notifyAll ()
            // The generator and the card are read at the start: it starts again for them.
            if generatorMoved && running then this.Restart()
            elif running then sendTimer.Change(200, Timeout.Infinite) |> ignore

    // Starting: with DLSS 5 on, LIVE FLOW is asked to hand its frames over first and the engine started once it
    // offers them (the link's memory is there), so it does not take the screen itself by mistake.
    let start () =
        if not busy && installed && ready () then
            busy <- true
            running <- true
            let share = shared ()
            linked <- share.Link
            startedOn <- share.Window
            notifyAll ()
            Task.Run(fun () ->
                if linked then
                    post (fun () -> setLink true)
                    let until = DateTime.UtcNow.AddSeconds 4.
                    while DateTime.UtcNow < until && linkState () = "off" do Thread.Sleep 100
                    Thread.Sleep 300
                else post (fun () -> setLink false)
                if cards.Length = 0 then cards <- engine.Cards()
                let ok = engine.Start(engineOptions (), { share with Link = linked })
                post (fun () ->
                    busy <- false
                    if not ok then
                        running <- false
                        setLink false
                    notifyAll ())) |> ignore

    let stop () =
        running <- false
        busy <- false
        engine.Stop()
        setLink false
        notifyAll ()

    do
        engine.Changed.Add(fun next ->
            post (fun () ->
                status <- next
                // The engine ended by itself (LIVE FLOW gone, the game's screen lost): the switch goes off.
                if next.Phase = SmfgIdle && running && not busy then
                    running <- false
                    wanted <- false
                    setLink false
                notify [ "StatusText"; "HasStatus"; "IsRunning"; "CounterBase"; "CounterShown"; "IsOn"; "IsOff"; "CanDownload" ]))

    let hotkey = new SmfgHotkey((0x0005 <<< 16) ||| 0x4D)   // Shift + Alt + M, as in MFG SUPER MOTION
    do hotkey.Pressed.Add(fun () -> post (fun () -> this.Toggle()))

    member _.Toggle() = if wanted then this.SwitchOff() else this.SwitchOn()
    member _.SwitchOn() =
        if installed then
            wanted <- true
            start ()
            notifyAll ()
    member _.SwitchOff() =
        wanted <- false
        stop ()
    /// Start again on the source that is right now (DLSS 5 switched, another generator or card).
    member _.Restart() =
        if running && not busy then
            busy <- true
            notifyAll ()
            Task.Run(fun () ->
                engine.StopAndWait 5000
                post (fun () ->
                    busy <- false
                    running <- false
                    if wanted then start () else setLink false
                    notifyAll ())) |> ignore

    /// LIVE FLOW's state moved: its shared switches go to the engine, and DLSS 5 on or off changes the source.
    member _.LiveFlowChanged() =
        // The card named, or AMD MODE switched: DLSS MFG offered or not.
        let dlss = dlssAllowed ()
        if dlssWas <> Some dlss then
            dlssWas <- Some dlss
            notifyAll ()
        if running && not busy then
            let share = shared ()
            if share.Link <> linked || share.Window <> startedOn then this.Restart()
            else
                match relinkCheck with
                | Some at when DateTime.UtcNow >= at ->
                    relinkCheck <- None
                    if linked && linkState () <> "linked" then this.Restart()
                    else sendTimer.Change(150, Timeout.Infinite) |> ignore
                | _ -> sendTimer.Change(150, Timeout.Infinite) |> ignore
    /// LIVE FLOW's worker was started again (round 42, "DLSS MFG stops"). Its link is a new one; the engine
    /// takes it by itself when it comes within 4 s, else it took the desktop itself - without DLSS 5, for good.
    /// 6 s on, an engine that is not taking LIVE FLOW's frames is started again (LiveFlowChanged).
    member _.LiveFlowRestarted() =
        if running && linked then relinkCheck <- Some(DateTime.UtcNow.AddSeconds 6.)
    /// LIVE FLOW stops (Exit, the page closed, the app closing).
    member _.LiveFlowStopped() =
        if running then
            wanted <- false
            stop ()

    // ---- the engine: there, missing, downloading ------------------------------
    member _.IsInstalled = installed
    member _.NeedsEngine = not installed
    member _.IsDownloading = downloading
    member _.DownloadPercent = progress * 100.0
    member _.EngineText =
        let loc = Localization.current
        if downloading then String.Format(loc.LfSmfgDownloading, int (progress * 100.0))
        elif failed <> "" then loc.LfSmfgFailed
        elif checking then loc.LfSmfgChecking
        elif not installed then loc.LfSmfgMissing
        else
            match latest, SmfgStore.installed () with
            | Some r, Some have when r.Version > have -> String.Format(loc.LfSmfgUpdate, r.Version.ToString())
            | _, Some have -> sprintf "%s %s" loc.LfSmfgEngine (have.ToString())
            | _ -> loc.LfSmfgEngine
    member _.HasEngineUpdate =
        match latest, SmfgStore.installed () with
        | Some r, Some have -> installed && r.Version > have
        | _ -> false
    // Not while the newest release is older than the engine this app needs (it would be downloaded for nothing).
    member _.CanDownload =
        not downloading && not running && (not installed || this.HasEngineUpdate)
        && (match latest with Some r -> r.Version >= SmfgStore.minimum | None -> true)
    /// Round 37: the engine comes with the app - it is only looked for, nothing is asked of GitHub.
    member _.CheckEngine() =
        installed <- SmfgStore.usable ()
        notifyAll ()
    member _.DownloadCommand = RelayCommand(fun () ->
        if not downloading && not running then
            downloading <- true
            failed <- ""
            progress <- 0.0
            notifyAll ()
            Task.Run(fun () ->
                let release =
                    match latest with
                    | Some r -> Some r
                    | None -> (SmfgReleases.latest CancellationToken.None).Result
                let result =
                    match release with
                    | None -> Error "no release"
                    | Some r -> (SmfgStore.install r (fun part -> post (fun () -> progress <- part; notify [ "DownloadPercent"; "EngineText" ])) CancellationToken.None).Result
                post (fun () ->
                    downloading <- false
                    latest <- release
                    match result with
                    | Ok () ->
                        installed <- SmfgStore.usable ()
                        cards <- [||]
                    | Error reason ->
                        failed <- reason
                        installed <- SmfgStore.usable ()
                    notifyAll ())) |> ignore)
    member _.OpenReleasesCommand = RelayCommand(fun () ->
        try Diagnostics.Process.Start(Diagnostics.ProcessStartInfo(SmfgReleases.page, UseShellExecute = true)) |> ignore with _ -> ())

    // ---- on / off and what the engine says --------------------------------------
    member _.IsOn = wanted
    member _.IsOff = not wanted
    member _.OnCommand = RelayCommand(fun () -> this.SwitchOn())
    member _.OffCommand = RelayCommand(fun () -> this.SwitchOff())
    member _.IsRunning = status.Phase = SmfgRunning
    member _.StatusText =
        let loc = Localization.current
        if not wanted then ""
        elif status.Error <> "" && status.Phase = SmfgIdle then String.Format(loc.LfSmfgError, status.Error)
        elif status.MfgUnavailable then loc.LfSmfgUnavailable
        elif status.Phase = SmfgRunning && status.BaseFps >= 0 then
            sprintf "%d → %d FPS · %s%s" status.BaseFps status.OutputFps (if status.Api = "dlss5" then "DLSS 5" else status.Api.ToUpperInvariant())
                (if status.VrrFixed then " · VRR!" else "")
        else loc.LfSmfgStarting
    member _.HasStatus = this.StatusText <> ""
    /// The FPS counter's figures while frames in between are made: the real rate and all shown, -1 none.
    member _.CounterBase = if wanted && status.Phase = SmfgRunning then status.BaseFps else -1
    member _.CounterShown = if wanted && status.Phase = SmfgRunning then status.OutputFps else -1
    member _.ShortcutText = "Shift + Alt + M"

    // ---- the options (MFG SUPER MOTION's Frame Generation, Upscaling and Advanced) ----
    member _.Generators = generatorIds () |> Array.map (fun g -> generatorNames[g]) |> keep "generators"
    member _.GeneratorIndex
        with get () = generatorIds () |> Array.tryFindIndex ((=) (generatorNow ())) |> Option.defaultValue 0
        and set value =
            let ids = generatorIds ()
            if not telling && value >= 0 && value < ids.Length then update (fun o -> { o with Generator = ids[value] })
    member _.IsDlss = generatorNow () = 0
    member _.IsXess = generatorNow () = 1
    member _.IsMsm = generatorNow () = 2
    member _.Modes = modesList
    member _.ModeIndex
        with get () = options.Mode
        and set value = if value >= 0 then update (fun o -> { o with Mode = value })
    member _.GameTypes = gameTypesList
    member _.GameTypeIndex
        with get () = options.GameType
        and set value = if value >= 0 then update (fun o -> { o with GameType = value })
    member _.IsFlowOpen = options.GameType = SmfgOptions.gameCustom
    member _.Flow
        with get () = float (SmfgOptions.effectiveFlow options)
        and set value =
            if Double.IsFinite value && options.GameType = SmfgOptions.gameCustom then
                update (fun o -> { o with Flow = int (Math.Round(Math.Clamp(value, 25., 100.) / 5.)) * 5 })
    member _.FlowLabel = sprintf "Flow  %d%%" (SmfgOptions.effectiveFlow options)
    member _.Upscalers = upscalersList
    member _.UpscalerIndex
        with get () = options.Upscaler
        and set value = if value >= 0 then update (fun o -> { o with Upscaler = value })
    member _.IsSharpnessOpen = options.Upscaler = 1 || options.Upscaler = 4
    member _.IsOverSharpnessOpen = options.Upscaler = 1
    member _.Sharpness
        with get () = float options.Sharpness
        and set value = if Double.IsFinite value then update (fun o -> { o with Sharpness = int (Math.Round(Math.Clamp(value, 0., 100.))) })
    member _.SharpnessLabel = sprintf "%s  %d%%" Localization.current.LfSmfgSharpness options.Sharpness
    member _.OverSharpness
        with get () = float options.OverSharpness
        and set value = if Double.IsFinite value then update (fun o -> { o with OverSharpness = int (Math.Round(Math.Clamp(value, 0., 100.))) })
    member _.OverSharpnessLabel = sprintf "%s  %d%%" Localization.current.LfSmfgOverSharpness options.OverSharpness
    member _.QueueTargets = queueTargetsList
    member _.QueueTargetIndex
        with get () = options.QueueTarget
        and set value = if value >= 0 then update (fun o -> { o with QueueTarget = value })
    member _.LimitToScreen = options.LimitToScreen
    member _.LimitToScreenOnCommand = RelayCommand(fun () -> update (fun o -> { o with LimitToScreen = true }))
    member _.LimitToScreenOffCommand = RelayCommand(fun () -> update (fun o -> { o with LimitToScreen = false }))
    member _.LowLatency = options.LowLatency
    member _.LowLatencyOnCommand = RelayCommand(fun () -> update (fun o -> { o with LowLatency = true }))
    member _.LowLatencyOffCommand = RelayCommand(fun () -> update (fun o -> { o with LowLatency = false }))
    member _.SyncModes = syncModesList
    member _.SyncModeIndex
        with get () = options.SyncMode
        and set value = if value >= 0 then update (fun o -> { o with SyncMode = value })
    /// How the movement is found: each generator has its own choices (MFG SUPER MOTION's three lists in one).
    member _.MotionChoices =
        (match generatorNow () with
         | 1 -> [| "XeSS MFG"; "GPU estimate"; "NVIDIA Optical Flow" |]
         | 2 -> [| "SMFG"; "SMFG · Fast" |]
         | _ -> [| "DLSS MFG"; "NVIDIA Optical Flow"; "GPU estimate" |])
        |> keep "motion"
    member _.MotionIndex
        with get () = match generatorNow () with | 1 -> options.XessMotionSource | 2 -> options.MsmSearch | _ -> options.MotionSource
        and set value =
            if not telling && value >= 0 then
                match generatorNow () with
                | 1 -> update (fun o -> { o with XessMotionSource = value })
                | 2 -> update (fun o -> { o with MsmSearch = value })
                | _ -> update (fun o -> { o with MotionSource = value })
    member _.StillPixels = options.StillPixels
    member _.StillPixelsOnCommand = RelayCommand(fun () -> update (fun o -> { o with StillPixels = true }))
    member _.StillPixelsOffCommand = RelayCommand(fun () -> update (fun o -> { o with StillPixels = false }))
    member _.MotionGuard = options.MotionGuard
    member _.MotionGuardOnCommand = RelayCommand(fun () -> update (fun o -> { o with MotionGuard = true }))
    member _.MotionGuardOffCommand = RelayCommand(fun () -> update (fun o -> { o with MotionGuard = false }))
    member _.GpuChoices = Array.append [| "Auto" |] (cardNames (generatorNow ())) |> keep "gpus"
    member _.GpuIndex
        with get () =
            match cardNames (generatorNow ()) |> Array.tryFindIndex ((=) options.Gpu) with
            | Some index -> index + 1
            | None -> 0
        and set value =
            let names = cardNames (generatorNow ())
            if not telling && value >= 0 && value <= names.Length then
                update (fun o -> { o with Gpu = (if value = 0 then "" else names[value - 1]) })
    // ---- round 36, from MFG SUPER MOTION's update -----------------------------------
    /// DLSS MFG's preset (NVIDIA's C, B, A), only with DLSS MFG.
    member _.DlssPresets = dlssPresetsList
    member _.DlssPresetIndex
        with get () = options.DlssPreset
        and set value = if not telling && value >= 0 && value <= 3 then update (fun o -> { o with DlssPreset = value })
    /// Performance (preset C) picked: the page says why it is not advised.
    member _.IsPresetCPicked = generatorNow () = 0 && options.DlssPreset = 1
    member _.GameFpsChoices = gameFpsList
    member _.GameFpsIndex
        with get () = SmfgOptions.gameFpsRates |> Array.tryFindIndex ((=) options.GameFps) |> Option.defaultValue 0
        and set value =
            if not telling && value >= 0 && value < SmfgOptions.gameFpsRates.Length then
                update (fun o -> { o with GameFps = SmfgOptions.gameFpsRates[value] })
    member _.GpuPriorities = gpuPrioritiesList
    member _.GpuPriorityIndex
        with get () = options.GpuPriority
        and set value = if value >= 0 then update (fun o -> { o with GpuPriority = value })
    member _.CaptureApis = captureApisList
    member _.CaptureApiIndex
        with get () = options.CaptureApi
        and set value = if value >= 0 then update (fun o -> { o with CaptureApi = value })

    /// The cards, asked of the engine once it is there (the generators and cards offered go by them).
    member _.LoadCards() =
        if installed && cards.Length = 0 then
            Task.Run(fun () ->
                let found = engine.Cards()
                post (fun () ->
                    cards <- found
                    notify [ "Generators"; "GeneratorIndex"; "GpuChoices"; "GpuIndex" ])) |> ignore

    interface IDisposable with
        member _.Dispose() =
            sendTimer.Dispose()
            (hotkey :> IDisposable).Dispose()
            (engine :> IDisposable).Dispose()
            setLink false

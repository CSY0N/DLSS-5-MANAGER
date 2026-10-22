namespace DLSS_5_MANAGER.Services

// SMFG in LIVE FLOW (round 33): MFG SUPER MOTION's frame generation, moved in
// as it is - its engine (downloaded, like the app downloads it for itself),
// its options, the way it is started and steered. Ported from MFG SUPER
// MOTION's app (ARMO\ARMO\Infrastructure: Updates.fs, MotionEngine.fs,
// GlobalHotkey.fs; Models\Preferences.fs). What is not here: picking a
// window - LIVE FLOW works on the whole screen, and so does SMFG - and the
// app's own pages (help, languages, profiles).
//
// With DLSS 5 on, LIVE FLOW's worker hands every finished frame to the engine
// on the card (ScreenNative\lf_msm_link.inl; the engine's sm_dlss5_link.inl),
// so the frames in between are made from DLSS 5's frames. With DLSS 5 off the
// engine takes the screen itself, exactly as MFG SUPER MOTION does.

open System
open System.Diagnostics
open System.Globalization
open System.IO
open System.IO.Compression
open System.Net.Http
open System.Runtime.InteropServices
open System.Security.Cryptography
open System.Text
open System.Text.Json
open System.Threading

/// SMFG's options: MFG SUPER MOTION's own (Models\Preferences.fs, DisplayOptions) less those that pick a window.
/// VRR, the frame limit, HDR, Spout and the FPS counter are LIVE FLOW's own switches, shared (SmfgEngine.Shared).
[<CLIMutable>]
type SmfgOptions =
    { Schema: int
      Generator: int         // 0 DLSS MFG, 1 XeSS MFG, 2 SMFG (MSM FG)
      Mode: int              // 0..4: x2..x6
      GameType: int          // index into SmfgOptions.gameFlows; the last one is "custom"
      Flow: int              // used with "custom"
      AdaptiveLearning: bool
      Upscaler: int          // 0 off, 1 sharpening only, 2 integer, 3 nearest neighbour, 4 Anime4K
      Sharpness: int
      OverSharpness: int
      QueueTarget: int
      LimitToScreen: bool
      LowLatency: bool
      SyncMode: int          // 0 off (tearing), 1 default, 2 VSync, 3 VSync 1/2, 4 VSync 1/3
      MotionSource: int      // DLSS MFG: 0 its own, 1 NVIDIA Optical Flow, 2 GPU estimate
      XessMotionSource: int  // XeSS MFG: 0 its own, 1 GPU estimate, 2 NVIDIA Optical Flow
      MsmSearch: int         // SMFG 1.0: 0 in full, 1 Fast
      StillPixels: bool
      MotionGuard: bool
      Gpu: string            // the card by name; "" the engine picks
      GpuPriority: int       // 0 realtime, 1 normal
      CaptureApi: int        // 0 DXGI, 1 WGC
      // Round 36, from MFG SUPER MOTION's update:
      DlssPreset: int        // DLSS MFG's preset: 0 the driver's own, 1 Performance (C), 2 Quality (B), 3 Balanced (A)
      GameFps: int }         // the game's frame rate as the person gives it (20..500); 0 the engine reads it from the frames

module SmfgOptions =
    let schema = 2
    let gameFlows = [| 35; 75; 50; 35; 35; 35; 35 |]
    let gameCustom = gameFlows.Length
    let generators = 3
    /// The Game FPS choices, as MFG SUPER MOTION offers them (0 = Auto).
    let gameFpsRates = [| 0; 30; 40; 45; 48; 50; 60; 72; 75; 90; 100; 120; 144; 165; 180; 200; 240; 280; 300; 360 |]
    let defaults =
        { Schema = schema; Generator = 0; Mode = 2; GameType = 0; Flow = 35; AdaptiveLearning = false
          Upscaler = 0; Sharpness = 50; OverSharpness = 0; QueueTarget = 1; LimitToScreen = false; LowLatency = true
          SyncMode = 1; MotionSource = 0; XessMotionSource = 0; MsmSearch = 1; StillPixels = true; MotionGuard = false
          Gpu = ""; GpuPriority = 0; CaptureApi = 0; DlssPreset = 2; GameFps = 0 }

    let effectiveFlow (o: SmfgOptions) = if o.GameType >= 0 && o.GameType < gameCustom then gameFlows[o.GameType] else o.Flow

    let normalize (o: SmfgOptions) =
        if isNull (box o) || o.Schema <= 0 then defaults
        else
            { o with Generator = Math.Clamp(o.Generator, 0, generators - 1)
                     Mode = Math.Clamp(o.Mode, 0, 4)
                     GameType = Math.Clamp(o.GameType, 0, gameCustom)
                     Flow = Math.Clamp(o.Flow, 25, 100)
                     Upscaler = (if o.Upscaler >= 0 && o.Upscaler < 5 then o.Upscaler else 0)
                     Sharpness = Math.Clamp(o.Sharpness, 0, 100)
                     OverSharpness = Math.Clamp(o.OverSharpness, 0, 100)
                     QueueTarget = Math.Clamp(o.QueueTarget, 0, 2)
                     SyncMode = (if o.SyncMode >= 0 && o.SyncMode < 5 then o.SyncMode else 1)
                     MotionSource = Math.Clamp(o.MotionSource, 0, 2)
                     XessMotionSource = Math.Clamp(o.XessMotionSource, 0, 2)
                     MsmSearch = Math.Clamp(o.MsmSearch, 0, 1)
                     Gpu = (if isNull o.Gpu then "" else o.Gpu.Trim())
                     GpuPriority = Math.Clamp(o.GpuPriority, 0, 1)
                     CaptureApi = Math.Clamp(o.CaptureApi, 0, 1)
                     // Saved before round 36: Quality, as MFG SUPER MOTION starts on it.
                     DlssPreset = (if o.Schema < 2 then 2 else Math.Clamp(o.DlssPreset, 0, 3))
                     GameFps = (if o.GameFps >= 20 && o.GameFps <= 500 then o.GameFps else 0)
                     MotionGuard = false
                     Schema = schema }

    let folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DLSS5Manager", "SuperMotion")
    let private file = Path.Combine(folder, "options.json")

    let load () =
        try
            if File.Exists file then normalize (JsonSerializer.Deserialize<SmfgOptions>(File.ReadAllText file))
            else defaults
        with _ -> defaults

    let save (o: SmfgOptions) =
        try
            Directory.CreateDirectory folder |> ignore
            let temp = file + ".tmp"
            File.WriteAllText(temp, JsonSerializer.Serialize(o, JsonSerializerOptions(WriteIndented = true)))
            File.Move(temp, file, true)
        with _ -> ()

/// The engine's release on GitHub: its version, its zip and the zip's SHA-256 when GitHub says it.
type SmfgRelease =
    { Version: Version
      Page: string
      Asset: string
      Size: int64
      Sha256: string }

/// Where the engine comes from: https://github.com/olivo-software/SUPER-MOTION/releases - the newest release's
/// zip (the engine, nvngx.dll, and what lies beside it), as MFG SUPER MOTION downloads it for itself.
module SmfgReleases =
    // The engine built for DLSS 5 MANAGER has its own releases (not MFG SUPER MOTION's).
    let repository = "olivo-software/super-motion-for-dlss-5-manager-"
    let page = "https://github.com/olivo-software/super-motion-for-dlss-5-manager-/releases"

    let mutable downloaded = 0L
    let mutable downloadSize = 0L

    let private client =
        lazy (
            let handler = new SocketsHttpHandler(ConnectTimeout = TimeSpan.FromSeconds 8., AutomaticDecompression = Net.DecompressionMethods.All)
            let client = new HttpClient(handler, Timeout = TimeSpan.FromSeconds 30.)
            client.DefaultRequestHeaders.UserAgent.ParseAdd "DLSS-5-MANAGER"
            client)

    /// "1.0.0", "v1.2", "engine-1.0.3": the numbers with dots in it, as a version.
    let parseVersion (tag: string) =
        if isNull tag then None
        else
            let digits = tag |> Seq.skipWhile (fun c -> not (Char.IsDigit c)) |> Seq.takeWhile (fun c -> Char.IsDigit c || c = '.') |> Seq.toArray |> String
            let parts = digits.Trim('.').Split('.', StringSplitOptions.RemoveEmptyEntries)
            if parts.Length = 0 || parts.Length > 4 then None
            else
                let padded = Array.append parts (Array.create (max 0 (3 - parts.Length)) "0")
                match Version.TryParse(String.Join(".", padded)) with
                | true, version -> Some version
                | _ -> None

    let private ask (address: string) (cancel: CancellationToken) =
        task {
            let mutable answer : HttpResponseMessage = null
            let mutable tries = 0
            while isNull answer && tries < 5 do
                tries <- tries + 1
                let! got =
                    task {
                        try
                            use limit = CancellationTokenSource.CreateLinkedTokenSource cancel
                            limit.CancelAfter(TimeSpan.FromSeconds 15.)
                            let! response = client.Value.GetAsync(address, limit.Token)
                            do! response.Content.LoadIntoBufferAsync()
                            return response
                        with _ -> return null
                    }
                answer <- got
                if isNull answer && tries < 5 then do! Threading.Tasks.Task.Delay(700, cancel)
            return answer
        }

    /// The newest release as GitHub's own pages say it (not its API: api.github.com cannot be reached from
    /// every network where github.com can - MFG SUPER MOTION's finding).
    let latest (cancel: CancellationToken) =
        task {
            try
                use! response = ask (sprintf "https://github.com/%s/releases/latest" repository) cancel
                if isNull response then return None else
                let landed = if isNull response.RequestMessage || isNull response.RequestMessage.RequestUri then "" else response.RequestMessage.RequestUri.AbsoluteUri
                let mark = "/releases/tag/"
                let at = landed.IndexOf(mark, StringComparison.OrdinalIgnoreCase)
                if not response.IsSuccessStatusCode || at < 0 then return None
                else
                    let tag = Uri.UnescapeDataString(landed.Substring(at + mark.Length).Split('?', '#').[0])
                    match parseVersion tag with
                    | None -> return None
                    | Some version ->
                        let! files =
                            task {
                                use! listing = ask (sprintf "https://github.com/%s/releases/expanded_assets/%s" repository (Uri.EscapeDataString tag)) cancel
                                if isNull listing || not listing.IsSuccessStatusCode then return ""
                                else return! listing.Content.ReadAsStringAsync(cancel)
                            }
                        let link = Text.RegularExpressions.Regex.Match(files, "href=\"(/[^\"]+/releases/download/[^\"]+\\.zip)\"", Text.RegularExpressions.RegexOptions.IgnoreCase)
                        let address =
                            if link.Success then "https://github.com" + Net.WebUtility.HtmlDecode link.Groups.[1].Value
                            else sprintf "https://github.com/%s/releases/download/%s/SUPER-MOTION-Engine-%s.zip" repository (Uri.EscapeDataString tag) (version.ToString())
                        let hash =
                            if link.Success then
                                let digest = Text.RegularExpressions.Regex.Match(files.Substring link.Index, "sha256:([0-9a-fA-F]{64})")
                                if digest.Success then digest.Groups.[1].Value else ""
                            else ""
                        return Some { Version = version; Page = landed; Asset = address; Size = 0L; Sha256 = hash }
            with _ -> return None
        }

    let private downloadPart (address: string) (target: string) (size: int64) (progress: float -> unit) (cancel: CancellationToken) =
        task {
            let have = if File.Exists target then FileInfo(target).Length else 0L
            use request = new HttpRequestMessage(HttpMethod.Get, address)
            if have > 0L then request.Headers.Range <- Net.Http.Headers.RangeHeaderValue(Nullable have, Nullable())
            use! response = client.Value.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancel)
            if have > 0L && response.StatusCode = Net.HttpStatusCode.RequestedRangeNotSatisfiable then return true
            else
                response.EnsureSuccessStatusCode() |> ignore
                let resumed = response.StatusCode = Net.HttpStatusCode.PartialContent
                let start = if resumed then have else 0L
                let rest = if response.Content.Headers.ContentLength.HasValue then response.Content.Headers.ContentLength.Value else 0L
                let total = if rest > 0L then start + rest else size
                downloadSize <- total
                use! from = response.Content.ReadAsStreamAsync(cancel)
                use into = new FileStream(target, (if resumed then FileMode.Append else FileMode.Create), FileAccess.Write, FileShare.None, 1 <<< 16, true)
                let buffer = Array.zeroCreate<byte> (1 <<< 16)
                let mutable got = start
                let mutable more = true
                while more do
                    use quiet = CancellationTokenSource.CreateLinkedTokenSource cancel
                    quiet.CancelAfter(TimeSpan.FromSeconds 40.)
                    let! read = from.ReadAsync(buffer.AsMemory(), quiet.Token)
                    if read = 0 then more <- false
                    else
                        do! into.WriteAsync(buffer.AsMemory(0, read), cancel)
                        got <- got + int64 read
                        downloaded <- got
                        if total > 0L then progress (min 1.0 (float got / float total))
                return total <= 0L || got >= total
        }

    /// A file downloaded to `target`; a connection that breaks is taken up again where the file ends, six goes in
    /// a row that bring nothing are the end of it.
    let download (address: string) (target: string) (size: int64) (progress: float -> unit) (cancel: CancellationToken) =
        task {
            downloaded <- 0L
            downloadSize <- size
            let mutable whole = false
            let mutable empty = 0
            while not whole do
                let before = if File.Exists target then FileInfo(target).Length else 0L
                let! ended =
                    task {
                        try
                            let! all = downloadPart address target size progress cancel
                            return (if all then 1 else 0)
                        with
                        | :? OperationCanceledException when cancel.IsCancellationRequested -> return 2
                        | _ -> return 0
                    }
                if ended = 2 then raise (OperationCanceledException())
                elif ended = 1 then whole <- true
                else
                    let after = if File.Exists target then FileInfo(target).Length else 0L
                    empty <- if after > before then 0 else empty + 1
                    if empty >= 6 then failwith "the connection was lost"
                    do! Threading.Tasks.Task.Delay(TimeSpan.FromSeconds 3., cancel)
        }

/// Where SMFG's engine is: "mod files\native\supermotion\Engine" beside the app (round 35; its version in
/// engine-version.txt), its log and caches in "...\supermotion\Run". AppData's DLSS5Manager\SuperMotion only when
/// "mod files" cannot be written to. SMFG_ENGINE names another folder (a build of the engine, tests).
module SmfgStore =
    let private appData = SmfgOptions.folder

    let private writable (dir: string) =
        try
            Directory.CreateDirectory dir |> ignore
            let probe = Path.Combine(dir, ".write-test")
            File.WriteAllText(probe, "")
            File.Delete probe
            true
        with _ -> false

    let rec private copyTree (from: string) (into: string) =
        Directory.CreateDirectory into |> ignore
        for file in Directory.GetFiles from do File.Copy(file, Path.Combine(into, Path.GetFileName file), true)
        for dir in Directory.GetDirectories from do copyTree dir (Path.Combine(into, Path.GetFileName dir))

    /// The folder SMFG keeps everything in. An engine downloaded before round 35 (in AppData) is moved over once
    /// rather than downloaded again; its old log and caches there are dropped.
    let private root =
        let preferred =
            match ModInstaller.modFilesRoot () with
            | null | "" -> ""
            | mods -> Path.Combine(mods, "native", "supermotion")
        if preferred <> "" && writable preferred then
            let oldEngine = Path.Combine(appData, "Engine")
            let newEngine = Path.Combine(preferred, "Engine")
            if Directory.Exists oldEngine && not (File.Exists(Path.Combine(newEngine, "nvngx.dll"))) then
                try
                    try Directory.Move(oldEngine, newEngine)
                    with _ ->
                        // Another drive: copied, then the old one goes.
                        copyTree oldEngine newEngine
                        Directory.Delete(oldEngine, true)
                with _ -> ()
            // Round 37: the engine comes with the app; a downloaded one left in AppData is not needed.
            elif Directory.Exists oldEngine then
                try Directory.Delete(oldEngine, true) with _ -> ()
            // The old log and caches are not needed where the engine no longer is.
            let oldRun = Path.Combine(appData, "Run")
            try (if Directory.Exists oldRun then Directory.Delete(oldRun, true)) with _ -> ()
            preferred
        else appData
    /// The engine's log, start file and caches (SUPER_MOTION_DATA).
    let dataFolder = Path.Combine(root, "Run")
    let folder =
        match Environment.GetEnvironmentVariable "SMFG_ENGINE" with
        | null | "" -> Path.Combine(root, "Engine")
        | other -> other
    let private versionFile = Path.Combine(folder, "engine-version.txt")
    let path () =
        let program = Path.Combine(folder, "nvngx.dll")
        if File.Exists program then Some program else None
    let installed () =
        try (if File.Exists versionFile then SmfgReleases.parseVersion (File.ReadAllText versionFile) else None) with _ -> None
    /// The first engine of DLSS 5 MANAGER's own releases (the whole screen without a pick, LIVE FLOW's frames).
    let minimum = Version(1, 0, 0)
    /// There and new enough (a build named with SMFG_ENGINE always is).
    let usable () =
        (path ()).IsSome
        && (not (String.IsNullOrEmpty(Environment.GetEnvironmentVariable "SMFG_ENGINE"))
            || (match installed () with Some have -> have >= minimum | None -> false))

    let private isRunning () =
        try
            Process.GetProcessesByName "nvngx"
            |> Array.exists (fun p -> try p.MainModule.FileName.StartsWith(folder, StringComparison.OrdinalIgnoreCase) with _ -> false)
        with _ -> false

    // The unpacked engine put where the engine is; what was there set aside first, and put back if that fails.
    let private place (from: string) (old: string) =
        if Directory.Exists folder then Directory.Move(folder, old)
        try
            Directory.Move(from, folder)
        with _ ->
            if Directory.Exists old && not (Directory.Exists folder) then Directory.Move(old, folder)
            reraise ()

    /// A release's zip downloaded, checked, unpacked and put in the engine's place. Error: what went wrong; the
    /// engine that was there is still there.
    let install (release: SmfgRelease) (progress: float -> unit) (cancel: CancellationToken) =
        task {
            let zip = Path.Combine(root, sprintf "engine-%s.zip.part" (release.Version.ToString()))
            let fresh = folder + ".new"
            let old = folder + ".old"
            let dropZip () = try (if File.Exists zip then File.Delete zip) with _ -> ()
            let clean () =
                try (if Directory.Exists fresh then Directory.Delete(fresh, true)) with _ -> ()
                try (if Directory.Exists old then Directory.Delete(old, true)) with _ -> ()
            try
                if String.IsNullOrEmpty release.Asset then return Error "the release has no zip file"
                elif isRunning () then return Error "the engine is running"
                else
                    Directory.CreateDirectory root |> ignore
                    clean ()
                    do! SmfgReleases.download release.Asset zip release.Size (fun part -> progress (part * 0.9)) cancel
                    let hashOk =
                        String.IsNullOrEmpty release.Sha256 ||
                        (use stream = File.OpenRead zip
                         String.Equals(Convert.ToHexString(SHA256.HashData stream), release.Sha256, StringComparison.OrdinalIgnoreCase))
                    if not hashOk then
                        dropZip (); clean ()
                        return Error "the downloaded file is not the one that was published"
                    else
                        let unpacked = try ZipFile.ExtractToDirectory(zip, fresh, true); true with _ -> false
                        progress 0.97
                        let found =
                            (if unpacked && Directory.Exists fresh then Directory.GetFiles(fresh, "nvngx.dll", SearchOption.AllDirectories) else [||])
                            |> Array.sortBy (fun file -> file.Length)
                            |> Array.tryHead
                        match found with
                        | None ->
                            dropZip (); clean ()
                            return Error "the zip holds no engine (nvngx.dll)"
                        | Some program ->
                            place (Path.GetDirectoryName program) old
                            File.WriteAllText(versionFile, release.Version.ToString())
                            dropZip (); clean ()
                            progress 1.0
                            return Ok()
            with
            | :? OperationCanceledException -> clean (); return Error "cancelled"
            | error -> clean (); return Error error.Message
        }

/// A graphics card as the engine lists it ("--cards").
type SmfgCard =
    { Index: int
      Dlss: bool
      Xess: bool
      Name: string }

/// What the engine is doing (its "@" lines).
type SmfgPhase =
    | SmfgIdle
    | SmfgStarting
    | SmfgRunning

type SmfgStatus =
    { Phase: SmfgPhase
      BaseFps: int
      OutputFps: int
      Width: int
      Height: int
      Api: string             // "dxgi", "wgc", "dlss5" (the frames come from LIVE FLOW)
      Multiplier: int
      MfgUnavailable: bool
      VrrFixed: bool
      Learning: int           // -1 not said, 0..100, 101 learnt
      Error: string }

module SmfgStatus =
    let idle =
        { Phase = SmfgIdle; BaseFps = -1; OutputFps = -1; Width = 0; Height = 0; Api = ""; Multiplier = 0
          MfgUnavailable = false; VrrFixed = false; Learning = -1; Error = "" }

/// LIVE FLOW's switches SMFG shares.
type SmfgShared =
    { Vrr: bool
      FrameLimit: int
      Hdr: bool
      Spout: bool
      Counter: bool           // LIVE FLOW's FPS counter on (and not held by the overlay's movable one)
      CounterX: int           // where LIVE FLOW's counter sits, from the screen's corner
      CounterY: int
      Monitor: int            // \\.\DISPLAYn, 0 the primary screen
      Link: bool              // DLSS 5 is on: the frames come from LIVE FLOW
      Window: int64 }         // round 37: the window picked with "Select window" (its handle), 0 the whole screen

/// Starts, steers and stops SMFG's engine (MotionEngine.fs of MFG SUPER MOTION).
type SmfgEngine() =
    let gate = obj ()
    let changed = Event<SmfgStatus>()
    let mutable proc: Process = null
    let mutable status = SmfgStatus.idle
    let mutable sent: (string * string) list = []
    let mutable stopping = false
    let mutable cards: SmfgCard array = [||]
    let mutable texts: (string * string) list = []
    let dataFolder = SmfgStore.dataFolder
    // The engine keeps its log and caches here, not in MFG SUPER MOTION's own folder.
    let ownFolder (info: ProcessStartInfo) =
        Directory.CreateDirectory dataFolder |> ignore
        info.Environment["SUPER_MOTION_DATA"] <- dataFolder
        info

    let publish next =
        let fire = lock gate (fun () -> if status = next then false else (status <- next; true))
        if fire then changed.Trigger next
    let update change = publish (change (lock gate (fun () -> status)))

    let optionLines (o: SmfgOptions) (shared: SmfgShared) =
        let flag value = if value then "1" else "0"
        let card =
            cards |> Array.tryFind (fun c -> c.Name = o.Gpu && (match o.Generator with | 1 -> c.Xess | 2 -> true | _ -> c.Dlss))
                  |> Option.map _.Index |> Option.defaultValue -1
        [ "generator", string o.Generator
          "gpu", string card
          "mfg", "1"
          "multiplier", string (o.Mode + 2)
          "flow", string (max 25 o.Flow)
          "game", string o.GameType
          "queue", string o.QueueTarget
          "still", flag o.StillPixels
          "guard", "0"                                    // MFG SUPER MOTION took the guard out
          "msm_search", string o.MsmSearch
          "aml", flag o.AdaptiveLearning
          "aml_flow", "0"
          // The engine's numbers are NVIDIA's order: 1 A (Balanced), 2 B (Quality), 3 C (Performance).
          "dlssg_preset", string ([| 0; 3; 2; 1 |][Math.Clamp(o.DlssPreset, 0, 3)])
          "game_fps", string o.GameFps
          "frame_limit", string shared.FrameLimit
          "screen_limit", flag o.LimitToScreen
          "vrr", flag shared.Vrr
          "sync", string o.SyncMode
          "low_latency", flag o.LowLatency
          "hdr", flag shared.Hdr
          "gpu_class", (if o.GpuPriority = 0 then "1" else "0")
          "counter", flag shared.Counter
          "counter_x", string shared.CounterX
          "counter_y", string shared.CounterY
          "spout", flag shared.Spout
          "upscaler", string o.Upscaler
          "sharpness", string o.Sharpness
          "over_sharpness", string o.OverSharpness
          "capture_api", string o.CaptureApi
          "motion", (match o.Generator with
                     | 1 -> (match o.XessMotionSource with | 1 -> "1" | 2 -> "0" | _ -> "2")
                     | 2 -> "2"
                     | _ -> (match o.MotionSource with | 1 -> "0" | 2 -> "1" | _ -> "2")) ]

    let number (text: string) =
        match Int32.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture) with
        | true, value -> value
        | _ -> -1

    let onLine (line: string) =
        if not (String.IsNullOrEmpty line) && line[0] = '@' then
            let parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries)
            match parts[0] with
            | "@state" when parts.Length > 1 ->
                match parts[1] with
                | "picking" | "starting" -> update (fun s -> { s with Phase = SmfgStarting })
                | "running" -> update (fun s -> { s with Phase = SmfgRunning })
                | _ -> ()
            | "@source" when parts.Length > 5 ->
                update (fun s -> { s with Width = number parts[2]; Height = number parts[3]; Api = parts[4] })
            | "@fps" when parts.Length > 2 -> update (fun s -> { s with BaseFps = number parts[1]; OutputFps = number parts[2] })
            | "@mfg" when parts.Length > 1 ->
                match parts[1] with
                | "off" -> update (fun s -> { s with Multiplier = 0; MfgUnavailable = false })
                | "unavailable" -> update (fun s -> { s with Multiplier = 0; MfgUnavailable = true })
                | value -> update (fun s -> { s with Multiplier = max 0 (number value); MfgUnavailable = false })
            | "@vrr" when parts.Length > 1 -> update (fun s -> { s with VrrFixed = parts[1] = "fixed" })
            | "@aml" when parts.Length > 1 ->
                let learning =
                    match parts[1] with
                    | "learned" -> 101
                    | "learning" when parts.Length > 2 -> Math.Clamp(number parts[2], 0, 100)
                    | _ -> -1
                update (fun s -> { s with Learning = learning })
            | "@error" when parts.Length > 1 -> update (fun s -> if s.Error = "" then { s with Error = parts[1] } else s)
            | _ -> ()

    let onExit (ended: Process) =
        let mine = lock gate (fun () ->
            if obj.ReferenceEquals(ended, proc) then
                proc <- null
                sent <- []
                true
            else false)
        if mine then
            let code = try ended.ExitCode with _ -> 0
            ended.Dispose()
            let last = lock gate (fun () -> status)
            let error =
                if last.Error <> "" then last.Error
                elif code <> 0 && not stopping then "failed"
                else ""
            publish { SmfgStatus.idle with Error = error }

    member _.Changed = changed.Publish
    member _.Status = lock gate (fun () -> status)
    member _.IsActive = lock gate (fun () -> not (isNull proc))
    member _.IsInstalled = (SmfgStore.path ()).IsSome
    member _.SetTexts(lines: (string * string) list) = lock gate (fun () -> texts <- lines)

    /// The cards as the engine sees them (asked once).
    member _.Cards() =
        match SmfgStore.path () with
        | Some program when cards.Length = 0 ->
            try
                let info =
                    ProcessStartInfo(FileName = program, Arguments = "--cards", WorkingDirectory = Path.GetDirectoryName program,
                                     UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true,
                                     StandardOutputEncoding = Encoding.UTF8)
                use listing = Process.Start(ownFolder info)
                let reading = listing.StandardOutput.ReadToEndAsync()
                let lines =
                    if reading.Wait 6000 then reading.Result.Split('\n')
                    else
                        (try listing.Kill() with _ -> ())
                        [||]
                listing.WaitForExit 1000 |> ignore
                let seen = Collections.Generic.Dictionary<string, int>()
                let refused =
                    lines |> Array.choose (fun line ->
                        let parts = line.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries)
                        if parts.Length = 3 && parts[0] = "@xess" && parts[2] = "0" then Some (number parts[1]) else None)
                cards <-
                    lines |> Array.choose (fun line ->
                        let parts = line.Trim().Split(' ', 5, StringSplitOptions.RemoveEmptyEntries)
                        if parts.Length = 5 && parts[0] = "@card" && number parts[1] >= 0 then
                            let name = parts[4].Trim()
                            let count = (match seen.TryGetValue name with | true, n -> n | _ -> 0) + 1
                            seen[name] <- count
                            Some { Index = number parts[1]; Dlss = parts[2] = "1"; Xess = not (Array.contains (number parts[1]) refused)
                                   Name = (if count > 1 then sprintf "%s (%d)" name count else name) }
                        else None)
            with _ -> ()
            cards
        | _ -> cards

    /// Start the engine on the whole screen. False when it cannot be started at all.
    member this.Start(options: SmfgOptions, shared: SmfgShared) =
        if this.IsActive then true
        else
            match SmfgStore.path () with
            | None ->
                publish { SmfgStatus.idle with Error = "missing" }
                false
            | Some program ->
                try
                    Directory.CreateDirectory dataFolder |> ignore
                    let lines = optionLines options shared
                    let startFile = Path.Combine(dataFolder, "start.cfg")
                    let text = StringBuilder()
                    for key, value in lines do text.Append(key).Append('=').AppendLine(value) |> ignore
                    for key, value in lock gate (fun () -> texts) do
                        text.Append(key).Append('=').AppendLine(value.Replace('\n', ' ').Replace('\r', ' ').Replace('#', ' ')) |> ignore
                    // The whole screen, nobody to click; LIVE FLOW's frames when DLSS 5 is on. With a window picked
                    // (round 37): that window - LIVE FLOW's own with DLSS 5 on (the link names it), else taken
                    // outright, borderless as MFG SUPER MOTION does.
                    if shared.Window <> 0L then
                        text.Append("source=0").AppendLine() |> ignore
                        if not shared.Link then
                            text.Append("hwnd=0x").AppendLine(shared.Window.ToString("X")) |> ignore
                            text.Append("window_mode=0").AppendLine() |> ignore
                    else
                        text.Append("source=1").AppendLine() |> ignore
                    text.Append("monitor=").AppendLine(string shared.Monitor) |> ignore
                    text.Append("delay=0").AppendLine() |> ignore
                    text.Append("link=").AppendLine(if shared.Link then "1" else "0") |> ignore
                    text.Append("owner=").AppendLine(string Environment.ProcessId) |> ignore
                    File.WriteAllText(startFile, text.ToString(), UTF8Encoding(false))
                    let info =
                        ProcessStartInfo(FileName = program, Arguments = sprintf "--config \"%s\"" startFile,
                                         WorkingDirectory = Path.GetDirectoryName program, UseShellExecute = false, CreateNoWindow = true,
                                         RedirectStandardInput = true, RedirectStandardOutput = true, StandardOutputEncoding = Encoding.UTF8)
                    let started = new Process(StartInfo = ownFolder info, EnableRaisingEvents = true)
                    started.OutputDataReceived.Add(fun args -> if not (isNull args.Data) then onLine args.Data)
                    started.Exited.Add(fun _ -> onExit started)
                    stopping <- false
                    publish { SmfgStatus.idle with Phase = SmfgStarting }
                    lock gate (fun () -> proc <- started; sent <- lines)
                    if not (started.Start()) then failwith "not started"
                    started.BeginOutputReadLine()
                    true
                with _ ->
                    lock gate (fun () -> proc <- null; sent <- [])
                    publish { SmfgStatus.idle with Error = "failed" }
                    false

    /// Send the controls that moved since the last time.
    member _.Send(options: SmfgOptions, shared: SmfgShared) =
        let running, changes =
            lock gate (fun () ->
                if isNull proc then null, []
                else
                    let lines = optionLines options shared
                    let before = dict sent
                    let changes = lines |> List.filter (fun (key, value) -> not (before.ContainsKey key) || before[key] <> value)
                    sent <- lines
                    proc, changes)
        if not (isNull running) && not changes.IsEmpty then
            try
                for key, value in changes do running.StandardInput.WriteLine(key + "=" + value)
                running.StandardInput.Flush()
            with :? IOException | :? InvalidOperationException | :? ObjectDisposedException -> ()

    /// Ask the engine to stop; ended when it will not.
    member _.Stop() =
        let running = lock gate (fun () -> proc)
        if not (isNull running) then
            stopping <- true
            try
                running.StandardInput.WriteLine "stop"
                running.StandardInput.Flush()
            with :? IOException | :? InvalidOperationException | :? ObjectDisposedException -> ()
            ThreadPool.QueueUserWorkItem(fun _ ->
                try
                    if not (running.WaitForExit 4000) then
                        try
                            use stop = EventWaitHandle.OpenExisting "Local\\SuperMotionEngineStop"
                            stop.Set() |> ignore
                        with _ -> ()
                        if not (running.WaitForExit 3000) then running.Kill()
                with _ -> ()) |> ignore

    /// Stop and wait (the app is closing, or the engine is started again for another source).
    member _.StopAndWait(milliseconds: int) =
        let running = lock gate (fun () -> proc)
        if not (isNull running) then
            stopping <- true
            try
                running.StandardInput.WriteLine "stop"
                running.StandardInput.Flush()
                if not (running.WaitForExit milliseconds) then running.Kill()
            with _ -> ()

    interface IDisposable with
        member this.Dispose() = this.StopAndWait 3000

module private SmfgHotkeyNative =
    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type MSG =
        val mutable hwnd: nativeint
        val mutable message: uint32
        val mutable wParam: nativeint
        val mutable lParam: nativeint
        val mutable time: uint32
        val mutable x: int
        val mutable y: int

    [<DllImport("user32.dll", SetLastError = true)>]
    extern bool RegisterHotKey(nativeint hWnd, int id, uint32 fsModifiers, uint32 vk)
    [<DllImport("user32.dll")>]
    extern bool UnregisterHotKey(nativeint hWnd, int id)
    [<DllImport("user32.dll", EntryPoint = "GetMessageW")>]
    extern int GetMessage(MSG& lpMsg, nativeint hWnd, uint32 wMsgFilterMin, uint32 wMsgFilterMax)
    [<DllImport("user32.dll", EntryPoint = "PeekMessageW")>]
    extern bool PeekMessage(MSG& lpMsg, nativeint hWnd, uint32 wMsgFilterMin, uint32 wMsgFilterMax, uint32 wRemoveMsg)
    [<DllImport("user32.dll", EntryPoint = "PostThreadMessageW")>]
    extern bool PostThreadMessage(uint32 idThread, uint32 Msg, nativeint wParam, nativeint lParam)
    [<DllImport("kernel32.dll")>]
    extern uint32 GetCurrentThreadId()
    let WM_HOTKEY = 0x0312u
    let WM_QUIT = 0x0012u
    let MOD_NOREPEAT = 0x4000u

/// SMFG's shortcut (MFG SUPER MOTION's: Shift + Alt + M), heard while a game has the keyboard: on a thread of its
/// own that only waits for Windows' message.
type SmfgHotkey(value: int) =
    let pressed = Event<unit>()
    let mutable threadId = 0u
    let run () =
        let mutable message = SmfgHotkeyNative.MSG()
        SmfgHotkeyNative.PeekMessage(&message, 0n, 0u, 0u, 0u) |> ignore
        threadId <- SmfgHotkeyNative.GetCurrentThreadId()
        let modifiers = (uint32 (value >>> 16) &&& 0xFu) ||| SmfgHotkeyNative.MOD_NOREPEAT
        let registered = SmfgHotkeyNative.RegisterHotKey(0n, 1, modifiers, uint32 (value &&& 0xFFFF))
        let mutable alive = true
        while alive do
            let result = SmfgHotkeyNative.GetMessage(&message, 0n, 0u, 0u)
            if result <= 0 then alive <- false
            elif message.message = SmfgHotkeyNative.WM_HOTKEY then pressed.Trigger()
        if registered then SmfgHotkeyNative.UnregisterHotKey(0n, 1) |> ignore
    let thread = Thread(run, IsBackground = true, Name = "smfg-hotkey")
    do thread.Start()
    member _.Pressed = pressed.Publish
    interface IDisposable with
        member _.Dispose() =
            if threadId <> 0u then SmfgHotkeyNative.PostThreadMessage(threadId, SmfgHotkeyNative.WM_QUIT, 0n, 0n) |> ignore

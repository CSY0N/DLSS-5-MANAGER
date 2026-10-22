namespace DLSS_5_MANAGER.Views

open Avalonia.Controls
open Avalonia.Markup.Xaml
open System
open Avalonia
open Avalonia.Controls.Primitives
open Avalonia.Controls.Shapes
open Avalonia.Data
open Avalonia.Input
open Avalonia.Interactivity
open Avalonia.Media
open Avalonia.Platform.Storage
open Avalonia.Threading
open Avalonia.VisualTree
open DLSS_5_MANAGER.Services
open DLSS_5_MANAGER.ViewModels

type ScreenView() as this =
    inherit UserControl()
    let mutable overlayInstance = false
    let mutable overlay: Window option = None
    let mutable hotkey: IDisposable option = None
    let mutable closing = false

    /// The bar being dragged, if any. A press that misses the handle still
    /// slides: the handle is invisible, and hunting for it would be the whole
    /// problem with the old bar all over again.
    let mutable sliding: Slider option = None

    /// The window's own view-model, which is where the translations live. The
    /// overlay is a separate window and would otherwise have none, leaving
    /// every label in it blank.
    let mutable ownerContext: obj = null

    /// The shortcut shown in the overlay's own header, kept current.
    let mutable overlayBadge: TextBlock option = None

    // The ids RegisterHotKey knows the two shortcuts by.
    let overlayHotkeyId = 0x5D51
    let dlssHotkeyId = 0x5D52
    let shotHotkeyId = 0x5D53
    let recordHotkeyId = 0x5D54

    /// Avalonia's key to the Windows virtual-key code RegisterHotKey wants.
    /// Letters, digits, the number pad and F1-F24 - everything a shortcut is
    /// reasonably made of. Anything else answers 0, which means "not usable".
    let virtualKeyOf (key: Key) : uint32 =
        let k = int key

        if key >= Key.A && key <= Key.Z then uint32 (0x41 + (k - int Key.A))
        elif key >= Key.D0 && key <= Key.D9 then uint32 (0x30 + (k - int Key.D0))
        elif key >= Key.NumPad0 && key <= Key.NumPad9 then uint32 (0x60 + (k - int Key.NumPad0))
        elif key >= Key.F1 && key <= Key.F24 then uint32 (0x70 + (k - int Key.F1))
        else 0u

    /// The round end of the fill is the handle (PillSlider in App.axaml): as
    /// wide as the bar is tall. It must match the thumb's width there, or a
    /// press would land a little off from where the fill then stops.
    let sliderHandle = 32.0

    let vm () = this.DataContext :?> ScreenViewModel

    // ---- FPS counter -------------------------------------------------------
    // The counter is drawn by the worker into the frame itself (round 28): a
    // window kept on top of the output made the compositor blend the two for
    // every frame. This small window is only its stand-in while the overlay
    // is open, so the counter can be dragged to another place; owned by the
    // section's own view (never the overlay's copy of it).
    let mutable counter: Window option = None
    let mutable counterWired = false
    /// True while the overlay is open: the counter takes clicks then, and can
    /// be dragged. At any other time clicks go through it to the game.
    let mutable counterMovable = false
    let counterTimer = DispatcherTimer(Interval = TimeSpan.FromMilliseconds(500.))

    let buildCounter (v: ScreenViewModel) =
        // MFG SUPER MOTION's look, as the worker and SMFG's engine draw it in the
        // frame (lf_counter.inl): "95" or "95/380", white with a thin dark edge.
        let figures (colour: IBrush) (dx: float) (dy: float) =
            let t =
                TextBlock(
                    FontFamily = FontFamily("Segoe UI"),
                    FontSize = 24.,
                    FontWeight = FontWeight.SemiBold,
                    Foreground = colour,
                    Margin = Thickness(2. + dx, 2. + dy, 2. - dx, 2. - dy)
                )

            t.Bind(TextBlock.TextProperty, Binding("CounterText")) |> ignore
            t

        let stack = Grid()
        for dx, dy in [ -2., 0.; 2., 0.; 0., -2.; 0., 2.; -1.5, -1.5; 1.5, -1.5; -1.5, 1.5; 1.5, 1.5 ] do
            stack.Children.Add(figures (SolidColorBrush(Color.FromArgb(217uy, 0uy, 0uy, 0uy))) dx dy)
        stack.Children.Add(figures Brushes.White 0. 0.)

        // Nearly clear, so it can still be taken hold of and dragged.
        let pill =
            Border(
                Name = "CounterPill",
                Background = SolidColorBrush(Color.Parse("#01000000")),
                Child = stack
            )

        let window =
            Window(
                Title = "DLSS 5 MANAGER FPS counter",
                SizeToContent = SizeToContent.WidthAndHeight,
                CanResize = false,
                Topmost = true,
                ShowInTaskbar = false,
                ShowActivated = false,
                Focusable = false,
                WindowDecorations = WindowDecorations.None,
                WindowStartupLocation = WindowStartupLocation.Manual,
                Background = Brushes.Transparent,
                DataContext = v,
                Content = pill
            )

        window.TransparencyLevelHint <- [ WindowTransparencyLevel.Transparent ]

        window.Opened.Add(fun _ ->
            let handle = window.TryGetPlatformHandle()

            if not (isNull handle) then
                ScreenDesktop.makeGhost handle.Handle
                ScreenDesktop.keepOnTop handle.Handle)

        // Movable only while it takes clicks (the overlay is open - see
        // setCounterMovable): press and drag, and it stays where it is let go.
        pill.PointerPressed.Add(fun e ->
            if counterMovable && e.GetCurrentPoint(pill).Properties.IsLeftButtonPressed then
                window.BeginMoveDrag(e))

        window.PositionChanged.Add(fun _ ->
            if counterMovable then
                let (x, y, _, _) = v.CounterArea
                v.MoveCounter(window.Position.X - x, window.Position.Y - y))

        window

    /// Up, placed and on top while it should show; hidden otherwise.
    let syncCounter () =
        match this.DataContext with
        | :? ScreenViewModel as v when v.IsCounterShown ->
            let window =
                match counter with
                | Some w -> w
                | None ->
                    let w = buildCounter v
                    counter <- Some w
                    // A new window starts click-through; the next tick makes it
                    // movable again if the overlay is open.
                    counterMovable <- false
                    w

            let (x, y, _, _) = v.CounterArea
            let (dx, dy) = v.CounterOffset
            let at = PixelPoint(x + dx, y + dy)
            if window.Position <> at && not counterMovable then window.Position <- at

            if not window.IsVisible then
                window.Show()
            else
                let handle = window.TryGetPlatformHandle()
                if not (isNull handle) then ScreenDesktop.keepOnTop handle.Handle
        | _ -> counter |> Option.iter (fun w -> if w.IsVisible then w.Hide())

    // ---- FLOW VIEW legend -----------------------------------------------------
    // What the colours mean, at the top-right of the output screen while FLOW
    // VIEW is on. Seen, never touched or captured - the same ghost as the counter.
    let mutable legend: Window option = None

    let buildLegend (v: ScreenViewModel) =
        let text (path: string) (size: float) (colour: string) (bold: bool) =
            let t =
                TextBlock(
                    FontSize = size,
                    FontWeight = (if bold then FontWeight.Bold else FontWeight.Normal),
                    Foreground = SolidColorBrush(Color.Parse(colour)),
                    TextWrapping = TextWrapping.Wrap,
                    MaxWidth = 250.,
                    VerticalAlignment = Layout.VerticalAlignment.Center
                )

            t.Bind(TextBlock.TextProperty, Binding(path)) |> ignore
            t

        let row (swatch: Control) (path: string) =
            let g = Grid(ColumnDefinitions = ColumnDefinitions("Auto,*"), ColumnSpacing = 10.)
            Grid.SetColumn(swatch, 0)
            let t = text path 11.5 "#E2E8F0" false
            Grid.SetColumn(t, 1)
            g.Children.Add(swatch)
            g.Children.Add(t)
            g

        let square (colour: string) =
            Border(Width = 18., Height = 12., CornerRadius = CornerRadius(3.),
                   Background = SolidColorBrush(Color.Parse(colour)), VerticalAlignment = Layout.VerticalAlignment.Center)

        // The motion's colour wheel, flattened: each colour is one direction.
        let wheel =
            let stops = GradientStops()
            for (offset, colour) in [ 0.0, "#FF3030"; 0.17, "#FFD000"; 0.33, "#40E040"; 0.5, "#20E0E0"
                                      0.67, "#3060FF"; 0.83, "#D040FF"; 1.0, "#FF3030" ] do
                stops.Add(GradientStop(Color.Parse(colour), offset))

            Border(Width = 18., Height = 12., CornerRadius = CornerRadius(3.),
                   Background = LinearGradientBrush(GradientStops = stops, StartPoint = RelativePoint(0., 0.5, RelativeUnit.Relative),
                                                    EndPoint = RelativePoint(1., 0.5, RelativeUnit.Relative)),
                   VerticalAlignment = Layout.VerticalAlignment.Center)

        let rows = StackPanel(Spacing = 7.)
        rows.Children.Add(text "Loc.LfFlowView" 12.5 "#FFFFFF" true)
        rows.Children.Add(row wheel "Loc.LfViewMotion")
        rows.Children.Add(row (square "#FFE01A") "Loc.LfViewCorrected")
        rows.Children.Add(row (square "#FF26D9") "Loc.LfViewHud")
        rows.Children.Add(row (square "#9959FF") "Loc.LfViewPanel")

        let window =
            Window(
                Title = "DLSS 5 MANAGER FLOW VIEW legend",
                SizeToContent = SizeToContent.WidthAndHeight,
                CanResize = false,
                Topmost = true,
                ShowInTaskbar = false,
                ShowActivated = false,
                Focusable = false,
                WindowDecorations = WindowDecorations.None,
                WindowStartupLocation = WindowStartupLocation.Manual,
                Background = Brushes.Transparent,
                DataContext = v,
                Content =
                    Border(
                        Background = SolidColorBrush(Color.Parse("#F20B1214")),
                        BorderBrush = SolidColorBrush(Color.Parse("#55E040C8")),
                        BorderThickness = Thickness(1.),
                        CornerRadius = CornerRadius(12.),
                        Padding = Thickness(14., 10.),
                        Child = rows
                    )
            )

        window.TransparencyLevelHint <- [ WindowTransparencyLevel.Transparent ]

        window.Opened.Add(fun _ ->
            let handle = window.TryGetPlatformHandle()

            if not (isNull handle) then
                ScreenDesktop.makeGhost handle.Handle
                ScreenDesktop.keepOnTop handle.Handle)

        window

    let syncLegend () =
        match this.DataContext with
        | :? ScreenViewModel as v when v.IsFlowLegendShown ->
            let window =
                match legend with
                | Some w -> w
                | None ->
                    let w = buildLegend v
                    legend <- Some w
                    w

            let (x, y, w, _) = v.CounterArea
            // Top-right: the counter keeps the other corner.
            let width = if window.Bounds.Width > 1. then int (window.Bounds.Width * window.RenderScaling) else 330
            let at = PixelPoint(x + max 0 (w - width - 20), y + 20)
            if window.Position <> at then window.Position <- at

            if not window.IsVisible then window.Show()
            else
                let handle = window.TryGetPlatformHandle()
                if not (isNull handle) then ScreenDesktop.keepOnTop handle.Handle
        | _ -> legend |> Option.iter (fun w -> if w.IsVisible then w.Hide())

    /// While the overlay is open the counter can be picked up: it takes clicks,
    /// shows a hand and a brighter edge. Closed, clicks go through it again.
    let setCounterMovable (movable: bool) =
        if movable <> counterMovable then
            counterMovable <- movable

            counter
            |> Option.iter (fun w ->
                let handle = w.TryGetPlatformHandle()
                if not (isNull handle) then ScreenDesktop.setClickThrough handle.Handle (not movable)

                match w.Content with
                | :? Border as pill ->
                    pill.Cursor <- (if movable then new Cursor(StandardCursorType.SizeAll) else Cursor.Default)
                    pill.BorderBrush <- SolidColorBrush(Color.Parse(if movable then "#FF34D399" else "#5500FF88"))
                    pill.BorderThickness <- Thickness(if movable then 1.5 else 1.)
                | _ -> ())

    do
        AvaloniaXamlLoader.Load(this)
        this.AddHandler(Button.ClickEvent, EventHandler<RoutedEventArgs>(fun _ _ -> UiSounds.tick()), RoutingStrategies.Bubble)

        // The page never scrolls itself.
        //
        // Anything that takes focus - a bar, a list, a switch - asks to be
        // brought into view, and the scroll viewer obliges. Focus also comes
        // back on its own: whenever the window is in front again (after the
        // output, the game or FLOW MFG's borderless window took the front),
        // the last bar or list touched gets focus once more and the page jumped
        // down to it ("it scrolls down on its own when I turn something on").
        // Nothing here needs to be scrolled to; the wheel and the bar still
        // work exactly as before.
        //
        // The page's own scroll viewer is inside this control, so the request
        // reaches it before it reaches the control: stopping it here (round 4)
        // was too late. The viewer no longer answers focus at all
        // (BringIntoViewOnFocusChange in the XAML), and the content stops any
        // other request before it gets to the viewer. Lists open in their own
        // popup and scroll inside it first, untouched.
        match this.FindControl<Border>("ScreenRoot") with
        | null -> ()
        | root ->
            root.AddHandler(
                Control.RequestBringIntoViewEvent,
                EventHandler<RequestBringIntoViewEventArgs>(fun _ e -> e.Handled <- true)
            )

        // Press anywhere on a bar and slide, the way a phone does it.
        //
        // Emptying the track's two halves of their hit-testing was not enough on
        // its own: the slider still treats a press on its background as a step
        // rather than a jump, and a press that never touched the handle starts
        // no drag. One handler for the whole section does both properly.
        let moveTo (slider: Slider) (x: float) =
            // The handle is as wide as the bar is tall and the track centres it
            // on the value, so the usable run is that much shorter and begins
            // half a handle in.
            let span = slider.Bounds.Width - sliderHandle

            if span > 1.0 then
                let ratio = Math.Clamp((x - sliderHandle / 2.0) / span, 0.0, 1.0)
                let tick = slider.TickFrequency

                if slider.IsSnapToTickEnabled && tick > 0.0 then
                    // Equal zones. Rounding to the nearest step gives the ends
                    // half the width of the middle - a three-step bar reads as
                    // six uneven parts. Thirds of the bar are thirds.
                    let steps = int (Math.Round((slider.Maximum - slider.Minimum) / tick)) + 1

                    if steps > 1 then
                        let index = Math.Clamp(int (ratio * float steps), 0, steps - 1)
                        slider.Value <- slider.Minimum + float index * tick
                else
                    slider.Value <- slider.Minimum + ratio * (slider.Maximum - slider.Minimum)

        this.AddHandler(
            InputElement.PointerPressedEvent,
            EventHandler<PointerPressedEventArgs>(fun _ e ->
                match e.Source with
                | :? Visual as source ->
                    let slider = source.FindAncestorOfType<Slider>(true)

                    if not (isNull slider) then
                        let point = e.GetCurrentPoint(slider)

                        if point.Properties.IsLeftButtonPressed then
                            sliding <- Some slider
                            moveTo slider point.Position.X
                | _ -> ()),
            RoutingStrategies.Bubble,
            true
        )

        this.AddHandler(
            InputElement.PointerMovedEvent,
            EventHandler<PointerEventArgs>(fun _ e ->
                match sliding with
                | Some slider ->
                    let point = e.GetCurrentPoint(slider)

                    if point.Properties.IsLeftButtonPressed then moveTo slider point.Position.X
                    else sliding <- None
                | None -> ()),
            RoutingStrategies.Bubble,
            true
        )

        this.AddHandler(
            InputElement.PointerReleasedEvent,
            EventHandler<PointerReleasedEventArgs>(fun _ _ -> sliding <- None),
            RoutingStrategies.Bubble,
            true
        )
        this.AttachedToVisualTree.Add(fun _ ->
            if not overlayInstance && hotkey.IsNone then
                match TopLevel.GetTopLevel(this) with
                | :? Window as owner ->
                    ownerContext <- owner.DataContext
                    // Both shortcuts belong to this section. Pressed while the
                    // window sits on GAMES or COMMUNITY they used to act anyway;
                    // what is already open can always be closed, though.
                    // Minimized or in the tray the window shows no section at
                    // all, and the shortcuts are the only way to LIVE FLOW
                    // (round 30). Started by Windows it sits there on GAMES -
                    // the overlay's key did nothing, and nothing said why.
                    let away () = not owner.IsVisible || owner.WindowState = WindowState.Minimized

                    let allowed () =
                        let isOpen =
                            match overlay with
                            | Some w -> w.IsVisible
                            | None -> false

                        match owner.DataContext with
                        | :? MainViewModel as main -> main.IsScreenTabActive || isOpen || away ()
                        | _ -> false

                    /// The overlay's key from the tray: LIVE FLOW becomes the
                    /// open section, so what the overlay shows is what the
                    /// window shows when it comes back.
                    let toLiveFlow () =
                        match owner.DataContext with
                        | :? MainViewModel as main when away () && not main.IsScreenTabActive -> main.ActiveSection <- "screen"
                        | _ -> ()

                    let register () =
                        if hotkey.IsNone && not closing then
                            let v = vm ()
                            let (overlayMods, overlayKey) = v.OverlayHotkey
                            let (dlssMods, dlssKey) = v.DlssHotkey
                            let (shotMods, shotKey) = v.ScreenshotHotkey
                            let (recMods, recKey) = v.RecordHotkey

                            let overlayBinding: ScreenDesktop.HotkeyBinding =
                                { Id = overlayHotkeyId
                                  Modifiers = overlayMods
                                  Key = overlayKey
                                  Action =
                                    (fun () ->
                                        Dispatcher.UIThread.Post(fun () ->
                                            if allowed () then
                                                toLiveFlow ()
                                                this.ToggleOverlay())) }

                            let dlssBinding: ScreenDesktop.HotkeyBinding =
                                { Id = dlssHotkeyId
                                  Modifiers = dlssMods
                                  Key = dlssKey
                                  Action = (fun () -> Dispatcher.UIThread.Post(fun () -> if allowed () then vm().ToggleNr())) }

                            let shotBinding: ScreenDesktop.HotkeyBinding =
                                { Id = shotHotkeyId
                                  Modifiers = shotMods
                                  Key = shotKey
                                  Action = (fun () -> Dispatcher.UIThread.Post(fun () -> if allowed () then vm().TakeScreenshot())) }

                            let recordBinding: ScreenDesktop.HotkeyBinding =
                                { Id = recordHotkeyId
                                  Modifiers = recMods
                                  Key = recKey
                                  Action = (fun () -> Dispatcher.UIThread.Post(fun () -> if allowed () then vm().ToggleRecording())) }

                            // Windows refuses a combination another program has
                            // already claimed. Say which, and where to change it.
                            let refused (ids: int list) =
                                Dispatcher.UIThread.Post(fun () ->
                                    let v = vm ()

                                    let names =
                                        [ if List.contains overlayHotkeyId ids then yield v.OverlayKeyText
                                          if List.contains dlssHotkeyId ids then yield v.DlssKeyText
                                          if List.contains shotHotkeyId ids then yield v.ScreenshotKeyText
                                          if List.contains recordHotkeyId ids then yield v.RecordKeyText ]

                                    v.ReportError(
                                        String.Join(", ", names)
                                        + " is already taken by another program. Pick a different one from its button."
                                    ))

                            hotkey <-
                                Some(new ScreenDesktop.HotkeyListener([ overlayBinding; dlssBinding; shotBinding; recordBinding ], refused) :> IDisposable)

                    // A changed shortcut takes effect at once: the old pair is
                    // released and the new pair registered in its place.
                    (vm ()).HotkeysChanged.Add(fun () ->
                        hotkey |> Option.iter (fun h -> h.Dispose())
                        hotkey <- None
                        register ()
                        overlayBadge |> Option.iter (fun badge -> badge.Text <- vm().OverlayKeyText))

                    // The FPS counter follows the session and the button.
                    if not counterWired then
                        counterWired <- true

                        (vm ()).PropertyChanged.Add(fun e ->
                            if e.PropertyName = "IsCounterShown" || e.PropertyName = "CounterArea" then syncCounter ()
                            if e.PropertyName = "" || e.PropertyName = "FlowView" || e.PropertyName = "FlowMfg" then syncLegend ())

                        counterTimer.Tick.Add(fun _ ->
                            let overlayOpen =
                                match overlay with
                                | Some w -> w.IsVisible
                                | None -> false

                            // The worker draws the counter; this window is
                            // only its movable stand-in while the overlay is open.
                            vm().HoldCounter overlayOpen
                            vm().RefreshCounterShown()
                            syncCounter ()
                            syncLegend ()
                            setCounterMovable overlayOpen)

                        counterTimer.Start()

                        owner.Closed.Add(fun _ ->
                            counterTimer.Stop()
                            counter |> Option.iter (fun w -> w.Close())
                            counter <- None
                            legend |> Option.iter (fun w -> w.Close())
                            legend <- None)

                    owner.Opened.Add(fun _ -> register())
                    if owner.IsVisible then register()
                    owner.Closed.Add(fun _ ->
                        closing <- true
                        hotkey |> Option.iter(fun h -> h.Dispose())
                        hotkey <- None
                        overlay |> Option.iter(fun w -> w.Close()))
                | _ -> ())

    member _.ConfigureOverlay() =
        overlayInstance <- true
        // The overlay's own card would only offer to open the overlay it is in.
        for name in ["NoResults"; "ScreenHeader"; "ComparisonCard"; "PresetsCard"; "SettingsCard"; "AmdNotice"
                     "OverlayCard"] do
            this.FindControl<Control>(name).IsVisible <- false
        // The overlay is where the picture is tuned while it is on screen, so
        // the colours belong here as much as the processing does. The
        // generator's card and FPS / VRR stay as the page has them (DLSS MFG
        // or FLOW MFG, by the card).
        for name in ["ProcessingCard"; "EffectCard"; "ColorsCard"; "CaptureCard"] do
            this.FindControl<Control>(name).IsVisible <- true

        // The main view keeps 90 pixels clear at the bottom for the window's
        // own navigation bar. Inside the overlay there is no such bar, so that
        // is just a band of empty panel under the last card.
        match this.FindControl<Border>("ScreenRoot") with
        | null -> ()
        | root -> root.Padding <- Thickness(18., 6., 18., 18.)

    member _.ToggleOverlay() =
        if not closing then
            vm().Start()
            match overlay with
            | Some window when window.IsVisible -> window.Hide()
            | Some window ->
                window.Show()
                window.Activate()
            | None ->
                let view = ScreenView(DataContext = this.DataContext)
                view.ConfigureOverlay()

                // The window itself is transparent and undecorated; the panel
                // people actually see is the Border below. That is the only way
                // to get rounded corners out of WindowDecorations.None - the
                // window is a bare rectangle whatever it is filled with.
                let window =
                    Window(
                        Title = "DLSS 5 MANAGER Screen overlay",
                        Width = 800.,
                        Height = 800.,
                        MinWidth = 720.,
                        MinHeight = 420.,
                        Topmost = true,
                        ShowInTaskbar = false,
                        WindowDecorations = WindowDecorations.None,
                        Background = Brushes.Transparent
                    )

                window.TransparencyLevelHint <- [ WindowTransparencyLevel.Transparent ]
                window.DataContext <- ownerContext

                let layout = Grid(RowDefinitions = RowDefinitions("Auto,*"))

                let header =
                    Grid(ColumnDefinitions = ColumnDefinitions("*,Auto,Auto"), Margin = Thickness(18., 12., 12., 6.))

                let titles =
                    StackPanel(
                        Orientation = Layout.Orientation.Horizontal,
                        Spacing = 9.,
                        VerticalAlignment = Layout.VerticalAlignment.Center
                    )

                titles.Children.Add(
                    TextBlock(
                        Text = "LIVE FLOW",
                        Foreground = Brushes.White,
                        FontWeight = FontWeight.Bold,
                        FontSize = 13.5,
                        LetterSpacing = 1.6,
                        VerticalAlignment = Layout.VerticalAlignment.Center
                    )
                )

                // The shortcut as it is set now - and kept current if it changes
                // while the overlay is open.
                let badge =
                    TextBlock(
                        Text = vm().OverlayKeyText,
                        FontSize = 10.5,
                        Foreground = SolidColorBrush(Color.Parse("#94A3B8"))
                    )

                overlayBadge <- Some badge

                titles.Children.Add(
                    Border(
                        Background = SolidColorBrush(Color.Parse("#14FFFFFF")),
                        CornerRadius = CornerRadius(8.),
                        Padding = Thickness(8., 2.),
                        VerticalAlignment = Layout.VerticalAlignment.Center,
                        Child = badge
                    )
                )

                header.Children.Add(titles)

                // The same FPS button as in the section's own bar. The window's
                // context is the main view-model (for the translations), so this
                // one is pointed at the section's own.
                let fps = Button(Padding = Thickness(12., 6.), Margin = Thickness(0., 0., 8., 0.), Focusable = false)
                fps.Classes.Add("AppleSecondaryBtn")
                fps.DataContext <- this.DataContext
                fps.Bind(ToolTip.TipProperty, Binding("Loc.LfFpsHint")) |> ignore

                let fpsRow = StackPanel(Orientation = Layout.Orientation.Horizontal, Spacing = 7.)

                let fpsDot =
                    Ellipse(
                        Width = 7.,
                        Height = 7.,
                        Fill = SolidColorBrush(Color.Parse("#34D399")),
                        VerticalAlignment = Layout.VerticalAlignment.Center
                    )

                fpsDot.Bind(Visual.IsVisibleProperty, Binding("IsFpsCounterOn")) |> ignore
                let fpsText = TextBlock(VerticalAlignment = Layout.VerticalAlignment.Center)
                fpsText.Bind(TextBlock.TextProperty, Binding("FpsButtonText")) |> ignore
                fpsRow.Children.Add(fpsDot)
                fpsRow.Children.Add(fpsText)
                fps.Content <- fpsRow

                fps.Click.Add(fun _ ->
                    UiSounds.tick ()
                    vm().ToggleFpsCounter())

                Grid.SetColumn(fps, 1)
                header.Children.Add(fps)

                let close = Button(Content = "\u2715", Padding = Thickness(12., 6.))
                close.Classes.Add("AppleSecondaryBtn")
                Grid.SetColumn(close, 2)

                close.Click.Add(fun _ ->
                    UiSounds.tickOff ()
                    window.Hide())

                header.Children.Add(close)
                Grid.SetRow(view, 1)
                layout.Children.Add(header)
                layout.Children.Add(view)

                let shell =
                    Border(
                        CornerRadius = CornerRadius(18.),
                        ClipToBounds = true,
                        Background = SolidColorBrush(Color.Parse("#F50B1214")),
                        BorderBrush = SolidColorBrush(Color.Parse("#2EFFFFFF")),
                        BorderThickness = Thickness(1.),
                        Child = layout
                    )

                window.Content <- shell

                // Dragging used to work from the header alone - a strip about
                // thirty pixels tall. Any empty space moves the panel now:
                // buttons, sliders and text boxes handle their own press
                // first, so this only ever sees what nothing else wanted.
                // Dragging from anywhere empty - but only from somewhere empty.
                //
                // Plenty of controls act on the *release*, not the press, so a
                // BeginMoveDrag on every press ran first and ate the click: the
                // profile and model lists stopped opening at all. Anything that
                // belongs to a control is left alone now.
                let isInteractive (source: obj) =
                    match source with
                    | :? Visual as v ->
                        not (isNull (v.FindAncestorOfType<Button>(true)))
                        || not (isNull (v.FindAncestorOfType<ComboBox>(true)))
                        || not (isNull (v.FindAncestorOfType<Slider>(true)))
                        || not (isNull (v.FindAncestorOfType<TextBox>(true)))
                        || not (isNull (v.FindAncestorOfType<CheckBox>(true)))
                        || not (isNull (v.FindAncestorOfType<Thumb>(true)))
                    | _ -> false

                let startDrag (e: PointerPressedEventArgs) =
                    if e.GetCurrentPoint(shell).Properties.IsLeftButtonPressed
                       && not (isInteractive e.Source) then
                        window.BeginMoveDrag(e)

                header.PointerPressed.Add(startDrag)
                shell.PointerPressed.Add(startDrag)

                let onTop = DispatcherTimer(Interval = TimeSpan.FromSeconds(2.))

                onTop.Tick.Add(fun _ ->
                    if window.IsVisible then
                        let handle = window.TryGetPlatformHandle()
                        if not (isNull handle) then ScreenDesktop.keepOnTop handle.Handle)

                window.Closing.Add(fun e ->
                    if not closing then
                        e.Cancel <- true
                        window.Hide())

                window.Opened.Add(fun _ ->
                    let handle = window.TryGetPlatformHandle()

                    if not (isNull handle) then
                        // WDA_NONE. This used to be WDA_EXCLUDEFROMCAPTURE
                        // (0x11), which is a standing instruction to Windows to
                        // leave the window out of every capture - which is
                        // exactly why the overlay never appeared in a
                        // screenshot or a recording.
                        ScreenDesktop.SetWindowDisplayAffinity(handle.Handle, 0u) |> ignore
                        ScreenDesktop.keepOnTop handle.Handle

                    onTop.Start())

                overlay <- Some window
                window.Show()
                window.Activate()

    member this.OnOpenOverlay(_: obj, _: RoutedEventArgs) = this.ToggleOverlay()

    /// A folded card's title: open it, or fold it again.
    member _.OnToggleSection(sender: obj, _: RoutedEventArgs) =
        match sender with
        | :? Control as c when not (isNull c.Tag) -> vm().ToggleSection(string c.Tag)
        | _ -> ()

    member _.OnToggleFpsCounter(_: obj, _: RoutedEventArgs) = vm().ToggleFpsCounter()

    member _.OnToggleFlowView(_: obj, _: RoutedEventArgs) = vm().ToggleFlowView()

    /// The key cap in a shortcut panel: click it, then press the combination.
    member _.OnCaptureHotkey(sender: obj, _: RoutedEventArgs) =
        match sender with
        | :? Control as c ->
            match c.Tag with
            | :? string as target ->
                vm().StartListening(target)
                c.Focus() |> ignore
            | _ -> ()
        | _ -> ()

    /// While a cap is listening, the next real key becomes the shortcut. The
    /// modifier keys on their own are ignored - they are waited past until the
    /// key they belong to arrives. Esc gives up without changing anything.
    member _.OnHotkeyPanelKeyDown(_: obj, e: KeyEventArgs) =
        let v = vm ()

        if v.Listening <> "" then
            if e.Key = Key.Escape then
                v.StopListening()
                e.Handled <- true
            else
                let vk = virtualKeyOf e.Key

                if vk <> 0u then
                    let mods =
                        (if e.KeyModifiers.HasFlag(KeyModifiers.Alt) then 1u else 0u)
                        ||| (if e.KeyModifiers.HasFlag(KeyModifiers.Control) then 2u else 0u)
                        ||| (if e.KeyModifiers.HasFlag(KeyModifiers.Shift) then 4u else 0u)
                        ||| (if e.KeyModifiers.HasFlag(KeyModifiers.Meta) then 8u else 0u)

                    let target = v.Listening
                    v.StopListening()
                    v.SetHotkey(target, mods, vk)
                    e.Handled <- true

    /// One of the ready-made combinations, carried as "target|modifiers|key".
    member _.OnHotkeyPreset(sender: obj, _: RoutedEventArgs) =
        match sender with
        | :? Control as c ->
            match c.Tag with
            | :? string as tag ->
                match tag.Split('|') with
                | [| target; mods; key |] ->
                    let v = vm ()
                    v.StopListening()
                    v.SetHotkey(target, uint32 mods, uint32 key)
                | _ -> ()
            | _ -> ()
        | _ -> ()
    member _.OnChooseOutput(_: obj, _: RoutedEventArgs) =
        task {
            try
                let owner = TopLevel.GetTopLevel(this)
                let! folders = owner.StorageProvider.OpenFolderPickerAsync(FolderPickerOpenOptions(Title="Screen capture folder", AllowMultiple=false))
                if folders.Count > 0 then
                    let path = folders[0].TryGetLocalPath()
                    if not (isNull path) then vm().OutputFolder <- path
            with ex -> vm().ReportError(ex.Message)
        } |> ignore

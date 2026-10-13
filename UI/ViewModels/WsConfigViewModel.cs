using Avalonia;
using Avalonia.Controls;
using Avalonia.VisualTree;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Mesen.Config;
using Mesen.Localization;
using Mesen.Utilities;
using Mesen.Windows;
using System;
using System.Threading.Tasks;

namespace Mesen.ViewModels
{
	public partial class WsConfigViewModel : DisposableViewModel
	{
		[ObservableProperty] public partial WsConfig Config { get; set; }
		[ObservableProperty] public partial WsConfig OriginalConfig { get; set; }
		[ObservableProperty] public partial WsConfigTab SelectedTab { get; set; } = 0;

		[ObservableProperty] public partial string WonderGateCode { get; set; } = "";
		[ObservableProperty] public partial string WonderGateStatus { get; set; } = "";
		[ObservableProperty] public partial bool WonderGateBusy { get; set; } = false;

		//Linked only counts for the server the token came from
		public bool WonderGateLinked => !string.IsNullOrEmpty(Config.WonderGateTokenForServer);
		public bool WonderGateUnlinked => !WonderGateLinked;
		public string WonderGateLinkedAs => ResourceHelper.GetMessage("WonderGateLinkedAs", string.IsNullOrEmpty(Config.WonderGateHandle) ? "?" : Config.WonderGateHandle, ServerHost);
		public bool WonderGateIsDefaultServer => WonderGateRelay.SameServer(Config.WonderGateServer, WsConfig.DefaultWonderGateServer);

		private string ServerHost => Uri.TryCreate(Config.WonderGateServer.Trim(), UriKind.Absolute, out Uri? uri) ? uri.Host : Config.WonderGateServer;

		public IAsyncRelayCommand LinkWonderGate { get; }
		public IRelayCommand UnlinkWonderGate { get; }
		public IRelayCommand UseDefaultWonderGateServer { get; }

		public IRelayCommand SetupPlayerHorizontal { get; }
		public IRelayCommand SetupPlayerVertical { get; }
		public IRelayCommand SetupPlayerPcv2 { get; }

		public WsConfigViewModel()
		{
			Config = ConfigManager.Config.Ws;
			OriginalConfig = Config.Clone();

			SetupPlayerHorizontal = new RelayCommand<Button>(btn => this.OpenSetup(btn!, ControllerType.WsController));
			SetupPlayerVertical = new RelayCommand<Button>(btn => this.OpenSetup(btn!, ControllerType.WsControllerVertical));
			SetupPlayerPcv2 = new RelayCommand<Button>(btn => this.OpenSetup(btn!, ControllerType.Pcv2Controller));
			LinkWonderGate = new AsyncRelayCommand(LinkWonderGateAsync);
			UnlinkWonderGate = new RelayCommand(() => SetWonderGateLink("", "", ""));
			UseDefaultWonderGateServer = new RelayCommand(() => Config.WonderGateServer = WsConfig.DefaultWonderGateServer);

			if(Design.IsDesignMode) {
				return;
			}

			AddDisposable(ReactiveHelper.RegisterObserver(Config, nameof(WsConfig.WonderGateServer), RefreshWonderGateLink));
			AddDisposable(ReactiveHelper.RegisterRecursiveObserver(Config, (s, e) => {
				Config.ApplyConfig();
				ConfigManager.Config.Video.ApplyConfig();
			}));
		}

		private async Task LinkWonderGateAsync()
		{
			if(string.IsNullOrWhiteSpace(WonderGateCode) || WonderGateBusy) {
				return;
			}
			WonderGateBusy = true;
			WonderGateStatus = ResourceHelper.GetMessage("WonderGateLinking");
			try {
				string server = Config.WonderGateServer.Trim();
				(string token, string handle) = await WonderGateRelay.Link(server, WonderGateCode, Environment.MachineName + " (MesenCE)");
				SetWonderGateLink(token, handle, server);
				WonderGateCode = "";
				WonderGateStatus = "";
			} catch(Exception ex) {
				WonderGateStatus = ResourceHelper.GetMessage("WonderGateLinkFailed", ex.Message);
			} finally {
				WonderGateBusy = false;
			}
		}

		private void SetWonderGateLink(string token, string handle, string server)
		{
			//Linking happens on the server right away, so keep it even if the window is cancelled
			Config.WonderGateToken = token;
			Config.WonderGateHandle = handle;
			Config.WonderGateTokenServer = server;
			OriginalConfig.WonderGateToken = token;
			OriginalConfig.WonderGateHandle = handle;
			OriginalConfig.WonderGateTokenServer = server;
			RefreshWonderGateLink();
		}

		private void RefreshWonderGateLink()
		{
			OnPropertyChanged(nameof(WonderGateLinked));
			OnPropertyChanged(nameof(WonderGateIsDefaultServer));
			OnPropertyChanged(nameof(WonderGateUnlinked));
			OnPropertyChanged(nameof(WonderGateLinkedAs));
		}

		private async void OpenSetup(Button btn, ControllerType type)
		{
			PixelPoint startPosition = btn.PointToScreen(new Point(-7, btn.Bounds.Height));
			ControllerConfigWindow wnd = new ControllerConfigWindow();
			ControllerConfig orgCfg = type switch {
				ControllerType.WsController => Config.ControllerHorizontal,
				ControllerType.WsControllerVertical => Config.ControllerVertical,
				ControllerType.Pcv2Controller => Config.ControllerPcv2,
				_ => throw new NotImplementedException()
			};

			ControllerConfig cfg = orgCfg.Clone();

			wnd.DataContext = new ControllerConfigViewModel(type, cfg, orgCfg, 0);

			if(await wnd.ShowDialogAtPosition<bool>(btn.GetWindow(), startPosition)) {
				switch(type) {
					case ControllerType.WsController: Config.ControllerHorizontal = cfg; break;
					case ControllerType.WsControllerVertical: Config.ControllerVertical = cfg; break;
					case ControllerType.Pcv2Controller: Config.ControllerPcv2 = cfg; break;
				}
			}
		}
	}

	public enum WsConfigTab
	{
		General,
		Audio,
		Emulation,
		Input,
		Video,
		WonderGate
	}
}

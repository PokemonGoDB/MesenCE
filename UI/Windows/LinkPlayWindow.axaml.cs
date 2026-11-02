using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using Mesen.Config;
using Mesen.Interop;
using Mesen.Localization;
using Mesen.Utilities;
using System;

namespace Mesen.Windows
{
	/// <summary>Link play: join a room by its code (rooms are opened and listed at swanrevival.com/link-play/), say
	/// when the console is ready, leave. The work is LinkRoomRelay's; this window only shows it.</summary>
	public class LinkPlayWindow : MesenWindow
	{
		public const string LobbyUrl = "https://swanrevival.com/link-play/";

		private readonly DispatcherTimer _timer;
		private readonly TextBox _code;
		private readonly TextBlock _status;
		private readonly TextBlock _detail;
		private readonly Button _join;
		private readonly Button _ready;
		private readonly Button _leave;
		private readonly Grid _joinPanel;

		public LinkPlayWindow()
		{
			AvaloniaXamlLoader.Load(this);
			_code = this.GetControl<TextBox>("txtCode");
			_status = this.GetControl<TextBlock>("lblStatus");
			_detail = this.GetControl<TextBlock>("lblDetail");
			_join = this.GetControl<Button>("btnJoin");
			_ready = this.GetControl<Button>("btnReady");
			_leave = this.GetControl<Button>("btnLeave");
			_joinPanel = this.GetControl<Grid>("pnlJoin");
			_timer = new DispatcherTimer(TimeSpan.FromMilliseconds(250), DispatcherPriority.Normal, (s, e) => Refresh());
			_timer.Start();
			Refresh();
		}

		protected override void OnClosed(EventArgs e)
		{
			_timer.Stop();
			base.OnClosed(e);
		}

		private void Lobby_OnClick(object sender, RoutedEventArgs e)
		{
			ApplicationHelper.OpenBrowser(LobbyUrl);
		}

		private void Join_OnClick(object sender, RoutedEventArgs e)
		{
			string? code = LinkRoomRelay.NormalizeCode(_code.Text ?? "");
			if(code == null) {
				_status.Text = ResourceHelper.GetMessage("LinkPlay_BadCode");
				_detail.Text = "";
				return;
			}
			WsConfig cfg = ConfigManager.Config.Ws;
			_ = LinkRoomRelay.Instance.Join(code, cfg.WonderGateServer, cfg.WonderGateTokenForServer, () => Dispatcher.UIThread.Post(PlugIn));
		}

		/// <summary>The room answered: plug the cable in, and power the console on again so the game sees it from the
		/// start (as with a real cable, which goes in before the console is switched on).</summary>
		public static void PlugIn()
		{
			ConfigManager.Config.Ws.ApplyConfig();
			if(EmuApi.IsRunning() && EmuApi.GetRomInfo().ConsoleType == ConsoleType.Ws) {
				LoadRomHelper.PowerCycle();
			}
		}

		private void Ready_OnClick(object sender, RoutedEventArgs e)
		{
			LinkRoomRelay.Instance.SetReady();
		}

		private void Leave_OnClick(object sender, RoutedEventArgs e)
		{
			LinkRoomRelay.Instance.Leave();
		}

		private void Refresh()
		{
			LinkRoomRelay r = LinkRoomRelay.Instance;
			bool inRoom = r.State == LinkRoomRelay.Stage.Joining || r.State == LinkRoomRelay.Stage.InRoom || r.State == LinkRoomRelay.Stage.Linked;
			_joinPanel.IsVisible = !inRoom;
			_leave.IsVisible = inRoom;
			_ready.IsVisible = r.State == LinkRoomRelay.Stage.InRoom;
			_ready.IsEnabled = r.Peer.Length > 0 && r.ConsoleIn && !r.Ready;

			string ping = r.PingMs is int ms ? ResourceHelper.GetMessage("LinkPlay_Ping", ms) : "";
			switch(r.State) {
				case LinkRoomRelay.Stage.Idle:
					_status.Text = ResourceHelper.GetMessage("LinkPlay_Idle");
					_detail.Text = "";
					break;
				case LinkRoomRelay.Stage.Joining:
					_status.Text = ResourceHelper.GetMessage("LinkPlay_Joining", r.Code);
					_detail.Text = "";
					break;
				case LinkRoomRelay.Stage.InRoom:
					_status.Text = ResourceHelper.GetMessage("LinkPlay_InRoom", r.Code, r.Title, r.Host);
					_detail.Text = !r.ConsoleIn
						? ResourceHelper.GetMessage("LinkPlay_LoadGame", r.Title)
						: r.Peer.Length == 0
							? ResourceHelper.GetMessage("LinkPlay_WaitingPeer") + ping
							: r.Ready
								? ResourceHelper.GetMessage("LinkPlay_WaitingReady", r.Peer) + ping
								: ResourceHelper.GetMessage("LinkPlay_PeerHere", r.Peer) + ping;
					break;
				case LinkRoomRelay.Stage.Linked:
					_status.Text = ResourceHelper.GetMessage("LinkPlay_Linked", r.Peer);
					_detail.Text = ResourceHelper.GetMessage("LinkPlay_Bytes", r.Sent, r.Received) + ping;
					break;
				case LinkRoomRelay.Stage.Ended:
					_status.Text = ResourceHelper.GetMessage("LinkPlay_Ended");
					_detail.Text = r.Problem.Length > 0 ? ResourceHelper.GetMessage(r.Problem) : "";
					break;
			}
		}
	}
}

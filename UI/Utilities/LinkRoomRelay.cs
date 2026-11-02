using Mesen.Config;
using Mesen.Interop;
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Mesen.Utilities;

/// <summary>
/// Link play: the WonderSwan's EXT port joined to another player's console through a room on the WonderGate server
/// (swanrevival.com/link-play/), as if by Bandai's link cable. This holds the room's WebSocket
/// (wss://host/room/CODE, signed in with the account's device token) and a local port for the core's link device
/// (WsLinkRoomDevice), which connects with 'L' 'K' 1 and then swaps [type][length LE16][payload] frames:
///   'S' start [flags: bit 0 keep the newer byte on overrun, bit 1 38400 baud][jitter allowance ms]
///   'X' stop;  'D' link data (6-byte records, passed to and from the room as they are)
/// The room protocol is in the site repo, workers/wondergate/src/room.ts. The device is silent until both players
/// are ready, as if no cable were plugged in.
/// </summary>
public sealed class LinkRoomRelay
{
	public static LinkRoomRelay Instance { get; } = new();

	public enum Stage { Idle, Joining, InRoom, Linked, Ended }

	private const byte JitterMs = 10;

	private readonly object _lock = new();
	private CancellationTokenSource? _cts;
	private ClientWebSocket? _ws;
	private TcpListener? _listener;
	private NetworkStream? _device;
	private readonly SemaphoreSlim _wsSend = new(1, 1);
	private readonly SemaphoreSlim _deviceSend = new(1, 1);
	private bool _autoReady;
	private bool _sentReady;

	// What the window shows. Read on any thread; Changed is raised after each change (not on the UI thread).
	public Stage State { get; private set; } = Stage.Idle;
	public string Code { get; private set; } = "";
	public string Game { get; private set; } = "";
	public string Title { get; private set; } = "";
	public string Host { get; private set; } = "";
	public string Peer { get; private set; } = "";
	public int Baud { get; private set; } = 9600;
	public bool KeepNewer { get; private set; }
	public bool ConsoleIn { get; private set; }
	public bool Ready => _sentReady;
	public int? PingMs { get; private set; }
	public long Sent { get; private set; }
	public long Received { get; private set; }
	/// <summary>Why the room ended, or what went wrong: a message id (resources: Messages, LinkPlay_...).</summary>
	public string Problem { get; private set; } = "";
	public event Action? Changed;

	/// <summary>The core's link device connects here while in a room (0 = not in one). Passed to the core by WsConfig.</summary>
	public ushort Port { get; private set; }

	/// <summary>A room code as typed (any case, spaces or a dash), or null if it can't be one.</summary>
	public static string? NormalizeCode(string text)
	{
		string code = text.Trim().ToUpperInvariant().Replace(" ", "").Replace("-", "");
		const string chars = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
		if(code.Length != 6) {
			return null;
		}
		foreach(char c in code) {
			if(chars.IndexOf(c) < 0) {
				return null;
			}
		}
		return code;
	}

	/// <summary>The room's address on the WonderGate server (wss://host/tunnel → wss://host/room/CODE).</summary>
	public static Uri? RoomUri(string server, string code)
	{
		if(!Uri.TryCreate(server.Trim(), UriKind.Absolute, out Uri? uri) || (uri.Scheme != "wss" && uri.Scheme != "ws")) {
			return null;
		}
		return new UriBuilder(uri) { Path = "/room/" + code, Query = "" }.Uri;
	}

	private void Notify() => Changed?.Invoke();

	/// <summary>Join a room. When the room answers, the console is power cycled with the link cable "plugged in"
	/// (onPlugIn), so the game sees the cable from the start. autoReady (the test runner) says ready as soon as
	/// there's a partner and the console is in.</summary>
	public async Task Join(string code, string server, string token, Action onPlugIn, bool autoReady = false)
	{
		Leave("");
		Uri? uri = RoomUri(server, code);
		if(uri == null) {
			Fail("LinkPlay_BadServer");
			return;
		}
		if(token.Length == 0) {
			Fail("LinkPlay_NeedLink");
			return;
		}

		CancellationTokenSource cts = new();
		ClientWebSocket ws = new();
		lock(_lock) {
			_cts = cts;
			_ws = ws;
			_autoReady = autoReady;
			_sentReady = false;
			Code = code;
			Game = Title = Host = Peer = Problem = "";
			PingMs = null;
			Sent = Received = 0;
			ConsoleIn = false;
			State = Stage.Joining;
		}
		Notify();

		try {
			await ws.ConnectAsync(uri, cts.Token);
			await SendText(ws, Hello(token), cts.Token);
		} catch(Exception ex) {
			EmuApi.WriteLogEntry("[Link play] Can't reach the room: " + ex.Message);
			Fail("LinkPlay_Unreachable");
			return;
		}
		_ = Task.Run(() => ReceiveLoop(ws, onPlugIn, cts.Token));
		_ = Task.Run(() => PingLoop(ws, cts.Token));
	}

	/// <summary>The player says their console is in the game's link menu.</summary>
	public void SetReady()
	{
		ClientWebSocket? ws = _ws;
		if(ws == null || _sentReady || State != Stage.InRoom || Peer.Length == 0) {
			return;
		}
		_sentReady = true;
		_ = SendText(ws, "{\"ready\":true}", CancellationToken.None);
		Notify();
	}

	/// <summary>Leave the room (problem: why, as a message id; "" for the player's own choice).</summary>
	public void Leave(string problem = "")
	{
		ClientWebSocket? ws;
		CancellationTokenSource? cts;
		lock(_lock) {
			ws = _ws;
			cts = _cts;
			_ws = null;
			_cts = null;
			if(ws == null && State == Stage.Idle) {
				return;
			}
		}
		_ = SendDevice((byte)'X', Array.Empty<byte>());
		try {
			if(ws?.State == WebSocketState.Open) {
				ws.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "leave", CancellationToken.None).Wait(500);
			}
		} catch { }
		cts?.Cancel();
		ws?.Dispose();
		StopListener();
		lock(_lock) {
			State = State == Stage.Idle ? Stage.Idle : Stage.Ended;
			if(problem.Length > 0 || Problem.Length == 0) {
				Problem = problem;
			}
			_sentReady = false;
		}
		ConfigManager.Config.Ws.ApplyConfig();
		Notify();
	}

	private void Fail(string problem)
	{
		lock(_lock) {
			Problem = problem;
		}
		Leave(problem);
		lock(_lock) {
			State = Stage.Ended;
		}
		Notify();
	}

	private async Task ReceiveLoop(ClientWebSocket ws, Action onPlugIn, CancellationToken ct)
	{
		byte[] buffer = new byte[8192];
		try {
			while(!ct.IsCancellationRequested && ws.State == WebSocketState.Open) {
				using MemoryStream message = new();
				WebSocketReceiveResult result;
				do {
					result = await ws.ReceiveAsync(buffer, ct);
					if(result.MessageType == WebSocketMessageType.Close) {
						if(State != Stage.Ended) {
							Fail(State == Stage.Linked ? "LinkPlay_Left" : "LinkPlay_Closed");
						}
						return;
					}
					message.Write(buffer, 0, result.Count);
				} while(!result.EndOfMessage);

				if(result.MessageType == WebSocketMessageType.Binary) {
					// The partner's records, straight to the console (the room only sends them after the start)
					byte[] records = message.ToArray();
					Received += records.Length / 6;
					await SendDevice((byte)'D', records);
					continue;
				}
				HandleText(Encoding.UTF8.GetString(message.ToArray()), onPlugIn);
			}
		} catch(Exception ex) when(!ct.IsCancellationRequested) {
			EmuApi.WriteLogEntry("[Link play] Lost the room: " + ex.Message);
			Fail("LinkPlay_Lost");
		} catch { }
	}

	private void HandleText(string text, Action onPlugIn)
	{
		JsonElement m;
		try {
			m = JsonDocument.Parse(text).RootElement;
		} catch {
			return;
		}
		if(m.ValueKind != JsonValueKind.Object) {
			return;
		}

		if(m.TryGetProperty("seat", out _)) {
			lock(_lock) {
				Code = Str(m, "room", Code);
				Game = Str(m, "game", "");
				Title = Str(m, "title", Game);
				Host = Str(m, "host", "");
				Baud = m.TryGetProperty("baud", out JsonElement b) && b.TryGetInt32(out int baud) ? baud : 9600;
				KeepNewer = Str(m, "overrun", "") == "new";
				State = Stage.InRoom;
			}
			EmuApi.WriteLogEntry($"[Link play] In room {Code} ({Game}, {Baud} baud)");
			StartListener();
			onPlugIn();
		} else if(m.TryGetProperty("peer", out JsonElement peer)) {
			lock(_lock) {
				Peer = peer.ValueKind == JsonValueKind.Object ? Str(peer, "handle", "") : "";
				// A partner who left before the start: both say ready again for the next one
				if(Peer.Length == 0) {
					_sentReady = false;
				}
			}
			MaybeAutoReady();
		} else if(m.TryGetProperty("start", out _)) {
			lock(_lock) {
				State = Stage.Linked;
			}
			byte flags = (byte)((KeepNewer ? 1 : 0) | (Baud == 38400 ? 2 : 0));
			_ = SendDevice((byte)'S', new byte[] { flags, JitterMs });
			EmuApi.WriteLogEntry("[Link play] Linked with " + Peer);
		} else if(m.TryGetProperty("pong", out JsonElement pong) && pong.TryGetDouble(out double sentAt)) {
			PingMs = (int)Math.Round(Environment.TickCount64 - sentAt);
		} else if(m.TryGetProperty("left", out _)) {
			Fail("LinkPlay_Left");
			return;
		} else if(m.TryGetProperty("error", out JsonElement error)) {
			string code = error.GetString() ?? "";
			Fail(code switch {
				"no-room" => "LinkPlay_NoRoom",
				"full" => "LinkPlay_Full",
				"sign-in" => "LinkPlay_NeedLink",
				"name" => "LinkPlay_NeedName",
				"kicked" => "LinkPlay_Kicked",
				"closed" => "LinkPlay_HostClosed",
				"limit" => "LinkPlay_Limit",
				"version" => "LinkPlay_Version",
				_ => "LinkPlay_Lost"
			});
			return;
		}
		Notify();
	}

	/// <summary>The room's first message. Written by hand: reflection-based JSON doesn't survive the AOT builds.</summary>
	private static string Hello(string token)
	{
		using MemoryStream buffer = new();
		using(Utf8JsonWriter w = new(buffer)) {
			w.WriteStartObject();
			w.WriteNumber("v", 1);
			w.WriteString("token", token);
			w.WriteEndObject();
		}
		return Encoding.UTF8.GetString(buffer.ToArray());
	}

	private static string Str(JsonElement e, string name, string fallback)
	{
		return e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? fallback : fallback;
	}

	private void MaybeAutoReady()
	{
		if(_autoReady && ConsoleIn && Peer.Length > 0) {
			SetReady();
		}
	}

	private async Task PingLoop(ClientWebSocket ws, CancellationToken ct)
	{
		while(!ct.IsCancellationRequested && ws.State == WebSocketState.Open) {
			try {
				await SendText(ws, "{\"ping\":" + Environment.TickCount64 + "}", ct);
				await Task.Delay(2000, ct);
			} catch {
				return;
			}
		}
	}

	private async Task SendText(ClientWebSocket ws, string text, CancellationToken ct)
	{
		await _wsSend.WaitAsync(ct);
		try {
			await ws.SendAsync(Encoding.UTF8.GetBytes(text), WebSocketMessageType.Text, true, ct);
		} finally {
			_wsSend.Release();
		}
	}

	// ---------- the core's link device ----------

	private void StartListener()
	{
		lock(_lock) {
			if(_listener != null) {
				return;
			}
			_listener = new TcpListener(IPAddress.Loopback, 0);
			_listener.Start();
			Port = (ushort)((IPEndPoint)_listener.LocalEndpoint).Port;
			TcpListener listener = _listener;
			CancellationToken ct = _cts?.Token ?? CancellationToken.None;
			_ = Task.Run(() => AcceptLoop(listener, ct));
		}
	}

	private void StopListener()
	{
		lock(_lock) {
			try { _listener?.Stop(); } catch { }
			_listener = null;
			Port = 0;
			try { _device?.Dispose(); } catch { }
			_device = null;
			ConsoleIn = false;
		}
	}

	private async Task AcceptLoop(TcpListener listener, CancellationToken ct)
	{
		while(!ct.IsCancellationRequested) {
			TcpClient client;
			try {
				client = await listener.AcceptTcpClientAsync(ct);
			} catch {
				return;
			}
			_ = Task.Run(() => DeviceLoop(client, ct));
		}
	}

	private async Task DeviceLoop(TcpClient client, CancellationToken ct)
	{
		using TcpClient tcp = client;
		tcp.NoDelay = true;
		NetworkStream stream = tcp.GetStream();
		try {
			byte[] hello = await ReadExactly(stream, 3, ct);
			if(hello[0] != 'L' || hello[1] != 'K' || hello[2] != 1) {
				return;
			}
			lock(_lock) {
				// A power cycle makes a new device: the newest one is the console's
				try { _device?.Dispose(); } catch { }
				_device = stream;
				ConsoleIn = true;
			}
			EmuApi.WriteLogEntry("[Link play] The console is plugged in");
			Notify();
			MaybeAutoReady();
			if(State == Stage.Linked) {
				// Plugged in again after the start (a reset): link it straight away
				byte flags = (byte)((KeepNewer ? 1 : 0) | (Baud == 38400 ? 2 : 0));
				await SendDevice((byte)'S', new byte[] { flags, JitterMs });
			}

			while(!ct.IsCancellationRequested) {
				byte[] head = await ReadExactly(stream, 3, ct);
				int length = head[1] | (head[2] << 8);
				byte[] payload = length > 0 ? await ReadExactly(stream, length, ct) : Array.Empty<byte>();
				if(head[0] == 'D' && payload.Length % 6 == 0 && _ws is ClientWebSocket ws && ws.State == WebSocketState.Open && State == Stage.Linked) {
					Sent += payload.Length / 6;
					await _wsSend.WaitAsync(ct);
					try {
						await ws.SendAsync(payload, WebSocketMessageType.Binary, true, ct);
					} finally {
						_wsSend.Release();
					}
				}
			}
		} catch {
		} finally {
			lock(_lock) {
				if(_device == stream) {
					_device = null;
					ConsoleIn = false;
				}
			}
			Notify();
		}
	}

	private async Task SendDevice(byte type, byte[] payload)
	{
		NetworkStream? device = _device;
		if(device == null) {
			return;
		}
		byte[] frame = new byte[3 + payload.Length];
		frame[0] = type;
		frame[1] = (byte)(payload.Length & 0xFF);
		frame[2] = (byte)(payload.Length >> 8);
		payload.CopyTo(frame, 3);
		await _deviceSend.WaitAsync();
		try {
			await device.WriteAsync(frame);
		} catch {
		} finally {
			_deviceSend.Release();
		}
	}

	private static async Task<byte[]> ReadExactly(NetworkStream stream, int count, CancellationToken ct)
	{
		byte[] data = new byte[count];
		for(int got = 0; got < count;) {
			int n = await stream.ReadAsync(data.AsMemory(got, count - got), ct);
			if(n <= 0) {
				throw new EndOfStreamException();
			}
			got += n;
		}
		return data;
	}
}

using Mesen.Config;
using Mesen.Interop;
using System;
using System.Buffers;
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
/// Carries the emulated MobileWonderGate adapter's connections to a WonderGate server.
/// The adapter (in the WonderSwan core) connects to a local port opened here and starts each
/// connection with 'W' 'G' port-high port-low (the port the game dialled). Each connection
/// becomes one WebSocket to the server's tunnel, which carries the bytes unchanged:
///   → {"v":1,"port":80,"token":"…"}   then binary frames both ways,
///   → {"eof":true} when the game stops sending; the server closes when the game's connection ends.
/// The games need byte-exact replies, which is why they're tunnelled rather than sent as HTTP.
/// </summary>
public sealed class WonderGateRelay
{
	public static WonderGateRelay Instance { get; } = new();

	private readonly object _lock = new();
	private CancellationTokenSource? _cts;
	private TcpListener? _listener;
	private string _server = "";
	private string _token = "";

	public bool Running => _cts != null;

	/// <summary>Raised once per run when a game reaches Swan Revival's server without a linked account.</summary>
	public event Action? UnlinkedConnection;
	private int _unlinkedRaised;
	public ushort Port { get; private set; }

	/// <summary>Start or stop to match the settings. Safe to call on every settings change.</summary>
	public void Update(WsConfig cfg)
	{
		lock(_lock) {
			_server = cfg.WonderGateServer.Trim();
			_token = cfg.WonderGateTokenForServer.Trim();

			if(!cfg.WonderGateEnabled || !Uri.TryCreate(_server, UriKind.Absolute, out Uri? uri) || (uri.Scheme != "wss" && uri.Scheme != "ws")) {
				Stop();
				return;
			}
			if(Running) {
				return;
			}

			try {
				_cts = new CancellationTokenSource();
				_listener = new TcpListener(IPAddress.Loopback, 0);
				_listener.Start();
				Port = (ushort)((IPEndPoint)_listener.LocalEndpoint).Port;
				TcpListener listener = _listener;
				CancellationToken ct = _cts.Token;
				_ = Task.Run(() => AcceptLoop(listener, ct));
				EmuApi.WriteLogEntry("[WonderGate] Relay ready for " + _server);
			} catch(Exception ex) {
				EmuApi.WriteLogEntry("[WonderGate] Relay could not start: " + ex.Message);
				Stop();
			}
		}
	}

	private void Stop()
	{
		_cts?.Cancel();
		_cts = null;
		try { _listener?.Stop(); } catch { }
		_listener = null;
		Port = 0;
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
			_ = Task.Run(() => Bridge(client, ct));
		}
	}

	private async Task Bridge(TcpClient client, CancellationToken ct)
	{
		string server, token;
		lock(_lock) {
			server = _server;
			token = _token;
		}

		using ClientWebSocket ws = new();
		using TcpClient tcp = client;
		tcp.NoDelay = true;
		try {
			//'W' 'G' port-high port-low: the port the game dialled
			NetworkStream stream = tcp.GetStream();
			byte[] preamble = new byte[4];
			for(int got = 0; got < 4;) {
				int n = await stream.ReadAsync(preamble.AsMemory(got, 4 - got), ct);
				if(n <= 0) {
					return;
				}
				got += n;
			}
			if(preamble[0] != 'W' || preamble[1] != 'G') {
				return;
			}
			int gamePort = (preamble[2] << 8) | preamble[3];

			await ws.ConnectAsync(new Uri(server), ct);
			await ws.SendAsync(Hello(gamePort, token), WebSocketMessageType.Text, true, ct);
			if(token.Length == 0 && SameServer(server, WsConfig.DefaultWonderGateServer) && Interlocked.Exchange(ref _unlinkedRaised, 1) == 0) {
				UnlinkedConnection?.Invoke();
			}

			Task up = Upload(stream, ws, ct);
			await Download(stream, ws, ct);
			//The server ended the game's connection: hang up on the adapter
			try { tcp.Client.Shutdown(SocketShutdown.Both); } catch { }
			await Task.WhenAny(up, Task.Delay(1000, ct));
		} catch(Exception ex) when(ex is not OperationCanceledException) {
			EmuApi.WriteLogEntry("[WonderGate] Can't reach " + server + ": " + ex.Message);
		} catch { }

		//Finish the close handshake, whichever side started it: when the server closed (the usual end of a
		//game's connection) the state is CloseReceived, and leaving without answering drops the connection,
		//which the server counts as a lost one
		if(ws.State == WebSocketState.Open || ws.State == WebSocketState.CloseReceived) {
			using CancellationTokenSource closeTimeout = new(2000);
			try { await ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "", closeTimeout.Token); } catch { }
		}
	}

	/// <summary>Adapter → server. When the adapter stops sending, tell the server (it may still be answering).</summary>
	private static async Task Upload(NetworkStream stream, ClientWebSocket ws, CancellationToken ct)
	{
		byte[] buffer = ArrayPool<byte>.Shared.Rent(4096);
		try {
			int n;
			while((n = await stream.ReadAsync(buffer.AsMemory(0, 4096), ct)) > 0) {
				await ws.SendAsync(buffer.AsMemory(0, n), WebSocketMessageType.Binary, true, ct);
			}
			if(ws.State == WebSocketState.Open) {
				await ws.SendAsync(Encoding.UTF8.GetBytes("{\"eof\":true}"), WebSocketMessageType.Text, true, ct);
			}
		} catch {
		} finally {
			ArrayPool<byte>.Shared.Return(buffer);
		}
	}

	/// <summary>Server → adapter, until the server closes.</summary>
	private static async Task Download(NetworkStream stream, ClientWebSocket ws, CancellationToken ct)
	{
		byte[] buffer = ArrayPool<byte>.Shared.Rent(8192);
		try {
			while(ws.State == WebSocketState.Open) {
				ValueWebSocketReceiveResult result = await ws.ReceiveAsync(buffer.AsMemory(0, 8192), ct);
				if(result.MessageType == WebSocketMessageType.Close) {
					break;
				}
				if(result.MessageType == WebSocketMessageType.Binary && result.Count > 0) {
					await stream.WriteAsync(buffer.AsMemory(0, result.Count), ct);
				}
				//Text messages are status ({"ready":true,"handle":…}); nothing to do with them here
			}
		} catch {
		} finally {
			ArrayPool<byte>.Shared.Return(buffer);
		}
	}

	private static byte[] Hello(int gamePort, string token)
	{
		using MemoryStream ms = new();
		using(Utf8JsonWriter w = new(ms)) {
			w.WriteStartObject();
			w.WriteNumber("v", 1);
			w.WriteNumber("port", gamePort);
			if(token.Length > 0) {
				w.WriteString("token", token);
			}
			w.WriteEndObject();
		}
		return ms.ToArray();
	}

	/// <summary>Same server, ignoring case and a trailing slash.</summary>
	public static bool SameServer(string a, string b)
	{
		return a.Trim().TrimEnd('/').Equals(b.Trim().TrimEnd('/'), StringComparison.OrdinalIgnoreCase) && a.Trim().Length > 0;
	}

	/// <summary>The server's account-link endpoint: https://host/link for wss://host/…, http for ws.</summary>
	public static string LinkUrl(string server)
	{
		Uri uri = new(server.Trim());
		string scheme = uri.Scheme == "ws" ? "http" : "https";
		return scheme + "://" + uri.Authority + "/link";
	}

	/// <summary>Swap a one-time code from the server's account page for a device token.</summary>
	public static async Task<(string token, string handle)> Link(string server, string code, string deviceName)
	{
		using MemoryStream ms = new();
		using(Utf8JsonWriter w = new(ms)) {
			w.WriteStartObject();
			w.WriteString("code", code.Trim());
			w.WriteString("name", deviceName);
			w.WriteEndObject();
		}

		using System.Net.Http.HttpClient client = new() { Timeout = TimeSpan.FromSeconds(20) };
		using System.Net.Http.ByteArrayContent content = new(ms.ToArray());
		content.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("application/json");
		using System.Net.Http.HttpResponseMessage res = await client.PostAsync(LinkUrl(server), content);
		string body = await res.Content.ReadAsStringAsync();

		using JsonDocument doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(body) ? "{}" : body);
		JsonElement root = doc.RootElement;
		if(!res.IsSuccessStatusCode || !root.TryGetProperty("token", out JsonElement tokenEl)) {
			string error = root.TryGetProperty("error", out JsonElement e) ? e.GetString() ?? "" : "HTTP " + (int)res.StatusCode;
			throw new Exception(error);
		}
		string handle = root.TryGetProperty("handle", out JsonElement h) && h.ValueKind == JsonValueKind.String ? h.GetString() ?? "" : "";
		return (tokenEl.GetString() ?? "", handle);
	}
}

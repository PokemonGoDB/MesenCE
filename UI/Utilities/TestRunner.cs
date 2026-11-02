using Mesen.Config;
using Mesen.Debugger.Utilities;
using Mesen.Interop;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Mesen.Utilities
{
	internal class TestRunner
	{
		internal static int Run(string[] args)
		{
			ConfigManager.DisableSaveSettings = true;
			CommandLineHelper commandLineHelper = new(args, true);

			if(commandLineHelper.FilesToLoad.Count != 1) {
				//No rom specified
				return -1;
			}

			EmuApi.InitDll();

			int timeout = commandLineHelper.TestRunnerTimeout;
			ConfigManager.Config.ApplyConfig();

			//Link play tests (the site repo's scripts): join MESEN_LINK_ROOM on MESEN_LINK_SERVER with MESEN_LINK_TOKEN
			//before the game loads, so the console starts with the cable in, and say ready once there's a partner
			string linkRoom = Environment.GetEnvironmentVariable("MESEN_LINK_ROOM") ?? "";
			if(linkRoom.Length > 0) {
				string server = Environment.GetEnvironmentVariable("MESEN_LINK_SERVER") ?? WsConfig.DefaultWonderGateServer;
				string token = Environment.GetEnvironmentVariable("MESEN_LINK_TOKEN") ?? "";
				bool plugged = false;
				LinkRoomRelay.Instance.Join(linkRoom, server, token, () => plugged = true, autoReady: true).Wait();
				Stopwatch wait = Stopwatch.StartNew();
				while(!plugged && LinkRoomRelay.Instance.State != LinkRoomRelay.Stage.Ended && wait.ElapsedMilliseconds < 10000) {
					System.Threading.Thread.Sleep(20);
				}
				if(!plugged) {
					Console.Error.WriteLine("link play: couldn't join " + linkRoom + " (" + LinkRoomRelay.Instance.Problem + ")");
					return -2;
				}
				ConfigManager.Config.ApplyConfig();
			}

			EmuApi.InitializeEmu(ConfigManager.HomeFolder, IntPtr.Zero, IntPtr.Zero, true, true, true, true);
			EmuApi.Pause();

			ConfigApi.SetEmulationFlag(EmulationFlags.ConsoleMode, true);

			if(!EmuApi.LoadRom(commandLineHelper.FilesToLoad[0], string.Empty)) {
				return -1;
			}

			DebugWorkspaceManager.Load();

			foreach(string luaScript in commandLineHelper.LuaScriptsToLoad) {
				try {
					string script = File.ReadAllText(luaScript);
					string path = (Path.GetDirectoryName(luaScript) ?? Program.OriginalFolder) + Path.DirectorySeparatorChar;
					int scriptId = DebugApi.LoadScript(luaScript, path, script);
				} catch { }
			}

			//Link play runs in real time, as two players would: MESEN_TESTRUNNER_SPEED=normal
			if(Environment.GetEnvironmentVariable("MESEN_TESTRUNNER_SPEED") != "normal") {
				ConfigApi.SetEmulationFlag(EmulationFlags.MaximumSpeed, true);
			}
			EmuApi.Resume();

			int result = -1;
			Stopwatch sw = Stopwatch.StartNew();
			while(sw.ElapsedMilliseconds < timeout * 1000) {
				System.Threading.Thread.Sleep(100);

				if(!EmuApi.IsRunning()) {
					result = EmuApi.GetStopCode();
					break;
				}
			}

			EmuApi.Stop();
			EmuApi.Release();
			return result;
		}
	}
}

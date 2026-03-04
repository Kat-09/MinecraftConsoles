#include "stdafx.h"

#include "Common/App_Defines.h"
#include "Common/Network/GameNetworkManager.h"
#include "Input.h"
#include "Minecraft.h"
#include "MinecraftServer.h"
#include "Options.h"
#include "Tesselator.h"
#include "Windows64/4JLibs/inc/4J_Render.h"
#include "Windows64/GameConfig/Minecraft.spa.h"
#include "Windows64/KeyboardMouseInput.h"
#include "Windows64/Network/WinsockNetLayer.h"
#include "Windows64/Windows64_UIController.h"

#include "../../Minecraft.World/AABB.h"
#include "../../Minecraft.World/Vec3.h"
#include "../../Minecraft.World/IntCache.h"
#include "../../Minecraft.World/TilePos.h"
#include "../../Minecraft.World/compression.h"
#include "../../Minecraft.World/OldChunkStorage.h"
#include "../../Minecraft.World/net.minecraft.world.level.tile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ATOM MyRegisterClass(HINSTANCE hInstance);
extern BOOL InitInstance(HINSTANCE hInstance, int nCmdShow);
extern HRESULT InitDevice();
extern void CleanupDevice();
extern void DefineActions(void);

extern HWND g_hWnd;
extern int g_iScreenWidth;
extern int g_iScreenHeight;
extern char g_Win64Username[17];
extern wchar_t g_Win64UsernameW[17];
extern ID3D11Device* g_pd3dDevice;
extern ID3D11DeviceContext* g_pImmediateContext;
extern IDXGISwapChain* g_pSwapChain;
extern ID3D11RenderTargetView* g_pRenderTargetView;
extern ID3D11DepthStencilView* g_pDepthStencilView;
extern DWORD dwProfileSettingsA[];

static const int kProfileValueCount = 5;
static const int kProfileSettingCount = 4;

struct DedicatedServerConfig
{
	int port;
	char bindIP[256];
	char name[17];
	int maxPlayers;
	__int64 seed;
	bool hasSeed;
	bool showHelp;
};

static volatile bool g_shutdownRequested = false;

static BOOL WINAPI ConsoleCtrlHandlerProc(DWORD ctrlType)
{
	switch (ctrlType)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		g_shutdownRequested = true;
		app.m_bShutdown = true;
		return TRUE;
	default:
		return FALSE;
	}
}

static int WaitForServerStoppedThreadProc(void *)
{
	if (g_NetworkManager.ServerStoppedValid())
	{
		g_NetworkManager.ServerStoppedWait();
	}
	return 0;
}

static void PrintUsage()
{
	printf("Minecraft.Server.exe [options]\n");
	printf("  -port <1-65535>       Listen TCP port (default: 25565)\n");
	printf("  -ip <addr>            Bind address (default: 0.0.0.0)\n");
	printf("  -bind <addr>          Alias of -ip\n");
	printf("  -name <name>          Host display name (max 16 chars)\n");
	printf("  -maxplayers <1-8>     Public slots (default: 8)\n");
	printf("  -seed <int64>         World seed\n");
	printf("  -help                 Show this help\n");
}

static void LogStartupStep(const char *message)
{
	printf("[startup] %s\n", message);
	fflush(stdout);
}

static bool ParseIntArg(const char *value, int *outValue)
{
	if (value == NULL || *value == 0)
		return false;

	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (end == value || *end != 0)
		return false;

	*outValue = (int)parsed;
	return true;
}

static bool ParseInt64Arg(const char *value, __int64 *outValue)
{
	if (value == NULL || *value == 0)
		return false;

	char *end = NULL;
	__int64 parsed = _strtoi64(value, &end, 10);
	if (end == value || *end != 0)
		return false;

	*outValue = parsed;
	return true;
}

static bool ParseCommandLine(int argc, char **argv, DedicatedServerConfig *config)
{
	for (int i = 1; i < argc; ++i)
	{
		const char *arg = argv[i];
		if (_stricmp(arg, "-help") == 0 || _stricmp(arg, "--help") == 0 || _stricmp(arg, "-h") == 0)
		{
			config->showHelp = true;
			return true;
		}
		else if ((_stricmp(arg, "-port") == 0) && (i + 1 < argc))
		{
			int port = 0;
			if (!ParseIntArg(argv[++i], &port) || port <= 0 || port > 65535)
			{
				printf("Invalid -port value.\n");
				return false;
			}
			config->port = port;
		}
		else if ((_stricmp(arg, "-ip") == 0 || _stricmp(arg, "-bind") == 0) && (i + 1 < argc))
		{
			strncpy_s(config->bindIP, sizeof(config->bindIP), argv[++i], _TRUNCATE);
		}
		else if ((_stricmp(arg, "-name") == 0) && (i + 1 < argc))
		{
			strncpy_s(config->name, sizeof(config->name), argv[++i], _TRUNCATE);
		}
		else if ((_stricmp(arg, "-maxplayers") == 0) && (i + 1 < argc))
		{
			int maxPlayers = 0;
			if (!ParseIntArg(argv[++i], &maxPlayers) || maxPlayers <= 0 || maxPlayers > MINECRAFT_NET_MAX_PLAYERS)
			{
				printf("Invalid -maxplayers value.\n");
				return false;
			}
			config->maxPlayers = maxPlayers;
		}
		else if ((_stricmp(arg, "-seed") == 0) && (i + 1 < argc))
		{
			if (!ParseInt64Arg(argv[++i], &config->seed))
			{
				printf("Invalid -seed value.\n");
				return false;
			}
			config->hasSeed = true;
		}
		else
		{
			printf("Unknown or incomplete argument: %s\n", arg);
			return false;
		}
	}

	return true;
}

static void SetExeWorkingDirectory()
{
	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(NULL, exePath, MAX_PATH);
	char *slash = strrchr(exePath, '\\');
	if (slash != NULL)
	{
		*(slash + 1) = 0;
		SetCurrentDirectoryA(exePath);
	}
}

int main(int argc, char **argv)
{
	DedicatedServerConfig config;
	config.port = WIN64_NET_DEFAULT_PORT;
	strncpy_s(config.bindIP, sizeof(config.bindIP), "0.0.0.0", _TRUNCATE);
	strncpy_s(config.name, sizeof(config.name), "DedicatedServer", _TRUNCATE);
	config.maxPlayers = MINECRAFT_NET_MAX_PLAYERS;
	config.seed = 0;
	config.hasSeed = false;
	config.showHelp = false;

	if (!ParseCommandLine(argc, argv, &config))
	{
		PrintUsage();
		return 1;
	}
	if (config.showHelp)
	{
		PrintUsage();
		return 0;
	}

	LogStartupStep("initializing process state");
	SetConsoleCtrlHandler(ConsoleCtrlHandlerProc, TRUE);
	SetExeWorkingDirectory();

	g_iScreenWidth = 1280;
	g_iScreenHeight = 720;

	strncpy_s(g_Win64Username, sizeof(g_Win64Username), config.name, _TRUNCATE);
	MultiByteToWideChar(CP_ACP, 0, g_Win64Username, -1, g_Win64UsernameW, 17);

	g_Win64MultiplayerHost = true;
	g_Win64MultiplayerJoin = false;
	g_Win64MultiplayerPort = config.port;
	strncpy_s(g_Win64MultiplayerIP, sizeof(g_Win64MultiplayerIP), config.bindIP, _TRUNCATE);

	LogStartupStep("registering hidden window class");
	HINSTANCE hInstance = GetModuleHandle(NULL);
	MyRegisterClass(hInstance);

	LogStartupStep("creating hidden window");
	if (!InitInstance(hInstance, SW_HIDE))
	{
		printf("Failed to create window instance.\n");
		return 2;
	}
	ShowWindow(g_hWnd, SW_HIDE);

	LogStartupStep("initializing graphics device wrappers");
	if (FAILED(InitDevice()))
	{
		printf("Failed to initialize D3D device.\n");
		CleanupDevice();
		return 2;
	}

	LogStartupStep("loading media/string tables");
	app.loadMediaArchive();
	RenderManager.Initialise(g_pd3dDevice, g_pSwapChain);
	app.loadStringTable();
	ui.init(g_pd3dDevice, g_pImmediateContext, g_pRenderTargetView, g_pDepthStencilView, g_iScreenWidth, g_iScreenHeight);

	InputManager.Initialise(1, 3, MINECRAFT_ACTION_MAX, ACTION_MAX_MENU);
	KMInput.Init(g_hWnd);
	DefineActions();
	InputManager.SetJoypadMapVal(0, 0);
	InputManager.SetKeyRepeatRate(0.3f, 0.2f);

	ProfileManager.Initialise(
		TITLEID_MINECRAFT,
		app.m_dwOfferID,
		PROFILE_VERSION_10,
		kProfileValueCount,
		kProfileSettingCount,
		dwProfileSettingsA,
		app.GAME_DEFINED_PROFILE_DATA_BYTES * XUSER_MAX_COUNT,
		&app.uiGameDefinedDataChangedBitmask);
	ProfileManager.SetDefaultOptionsCallback(&CConsoleMinecraftApp::DefaultOptionsCallback, (LPVOID)&app);
	ProfileManager.SetDebugFullOverride(true);

	LogStartupStep("initializing network manager");
	g_NetworkManager.Initialise();

	for (int i = 0; i < MINECRAFT_NET_MAX_PLAYERS; ++i)
	{
		IQNet::m_player[i].m_smallId = (BYTE)i;
		IQNet::m_player[i].m_isRemote = false;
		IQNet::m_player[i].m_isHostPlayer = (i == 0);
		swprintf_s(IQNet::m_player[i].m_gamertag, 32, L"Player%d", i);
	}
	wcscpy_s(IQNet::m_player[0].m_gamertag, 32, g_Win64UsernameW);
	WinsockNetLayer::Initialize();

	Tesselator::CreateNewThreadStorage(1024 * 1024);
	AABB::CreateNewThreadStorage();
	Vec3::CreateNewThreadStorage();
	IntCache::CreateNewThreadStorage();
	Compression::CreateNewThreadStorage();
	OldChunkStorage::CreateNewThreadStorage();
	Level::enableLightingCache();
	Tile::CreateNewThreadStorage();

	LogStartupStep("creating Minecraft singleton");
	Minecraft::main();
	Minecraft *minecraft = Minecraft::GetInstance();
	if (minecraft == NULL)
	{
		printf("Minecraft initialization failed.\n");
		CleanupDevice();
		return 3;
	}

	app.InitGameSettings();
	if (minecraft->options != NULL)
	{
		minecraft->options->set(Options::Option::MUSIC, 0.0f);
		minecraft->options->set(Options::Option::SOUND, 0.0f);
	}

	MinecraftServer::resetFlags();
	app.SetTutorialMode(false);
	app.SetCorruptSaveDeleted(false);
	app.SetGameHostOption(eGameHostOption_Difficulty, 1);
	app.SetGameHostOption(eGameHostOption_FriendsOfFriends, 0);
	app.SetGameHostOption(eGameHostOption_Gamertags, 1);
	app.SetGameHostOption(eGameHostOption_BedrockFog, 1);
	app.SetGameHostOption(eGameHostOption_GameType, 0);
	app.SetGameHostOption(eGameHostOption_LevelType, 0);
	app.SetGameHostOption(eGameHostOption_Structures, 1);
	app.SetGameHostOption(eGameHostOption_BonusChest, 0);
	app.SetGameHostOption(eGameHostOption_PvP, 1);
	app.SetGameHostOption(eGameHostOption_TrustPlayers, 1);
	app.SetGameHostOption(eGameHostOption_FireSpreads, 1);
	app.SetGameHostOption(eGameHostOption_TNT, 1);
	app.SetGameHostOption(eGameHostOption_HostCanFly, 1);
	app.SetGameHostOption(eGameHostOption_HostCanChangeHunger, 1);
	app.SetGameHostOption(eGameHostOption_HostCanBeInvisible, 1);

	NetworkGameInitData *param = new NetworkGameInitData();
	if (config.hasSeed)
	{
		param->seed = config.seed;
	}
	param->saveData = NULL;
	param->settings = app.GetGameHostOption(eGameHostOption_All);
	param->dedicatedNoLocalHostPlayer = true;

	LogStartupStep("starting hosted network game thread");
	g_NetworkManager.HostGame(0, true, false, (unsigned char)config.maxPlayers, 0);
	g_NetworkManager.FakeLocalPlayerJoined();

	C4JThread *startThread = new C4JThread(&CGameNetworkManager::RunNetworkGameThreadProc, (LPVOID)param, "RunNetworkGame");
	startThread->Run();

	while (startThread->isRunning() && !g_shutdownRequested)
	{
		g_NetworkManager.DoWork();
		ProfileManager.Tick();
		StorageManager.Tick();
		Sleep(10);
	}

	startThread->WaitForCompletion(INFINITE);
	int startupResult = startThread->GetExitCode();
	delete startThread;

	if (startupResult != 0)
	{
		printf("Failed to start dedicated server (code %d).\n", startupResult);
		WinsockNetLayer::Shutdown();
		g_NetworkManager.Terminate();
		CleanupDevice();
		return 4;
	}

	LogStartupStep("server startup complete");
	printf("Dedicated server listening on %s:%d\n", g_Win64MultiplayerIP, g_Win64MultiplayerPort);

	while (!g_shutdownRequested && !app.m_bShutdown)
	{
		g_NetworkManager.DoWork();
		ProfileManager.Tick();
		StorageManager.Tick();
		app.HandleXuiActions();

		if (MinecraftServer::serverHalted())
		{
			break;
		}

		Sleep(10);
	}

	printf("Stopping dedicated server...\n");
	MinecraftServer::HaltServer();

	if (g_NetworkManager.ServerStoppedValid())
	{
		C4JThread waitThread(&WaitForServerStoppedThreadProc, NULL, "WaitServerStopped");
		waitThread.Run();
		waitThread.WaitForCompletion(INFINITE);
	}

	WinsockNetLayer::Shutdown();
	g_NetworkManager.Terminate();
	CleanupDevice();

	return 0;
}

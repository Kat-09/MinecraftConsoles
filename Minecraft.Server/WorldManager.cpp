#include "stdafx.h"

#include "WorldManager.h"

#include "Minecraft.h"
#include "MinecraftServer.h"
#include "ServerLogger.h"
#include "Common\\StringUtils.h"

#include <stdio.h>
#include <string.h>

namespace ServerRuntime
{
using StringUtils::Utf8ToWide;
using StringUtils::WideToUtf8;

enum EWorldSaveLoadResult
{
	eWorldSaveLoad_Loaded,
	eWorldSaveLoad_NotFound,
	eWorldSaveLoad_Failed
};

struct SaveInfoQueryContext
{
	bool done;
	bool success;
	SAVE_DETAILS *details;

	SaveInfoQueryContext()
		: done(false)
		, success(false)
		, details(NULL)
	{
	}
};

struct SaveDataLoadContext
{
	bool done;
	bool isCorrupt;
	bool isOwner;

	SaveDataLoadContext()
		: done(false)
		, isCorrupt(true)
		, isOwner(false)
	{
	}
};

/**
 * `StorageManager` に保存先ID（`level-id`）を反映する
 *
 * - 起動直後や保存直前に毎回同じIDを設定し、保存先のブレを防ぐ
 * - 空文字は無効値として無視する
 *
 * @param saveFilename 正規化済みの保存先ID
 */
static void SetStorageSaveUniqueFilename(const std::string &saveFilename)
{
	if (saveFilename.empty())
	{
		return;
	}

	char filenameBuffer[64] = {};
	strncpy_s(filenameBuffer, sizeof(filenameBuffer), saveFilename.c_str(), _TRUNCATE);
	StorageManager.SetSaveUniqueFilename(filenameBuffer);
}

static void LogSaveFilename(const char *prefix, const std::string &saveFilename)
{
	LogInfof("world-io", "%s: %s", (prefix != NULL) ? prefix : "save-filename", saveFilename.c_str());
}

static void LogEnumeratedSaveInfo(int index, const SAVE_INFO &saveInfo)
{
	std::wstring title = Utf8ToWide(saveInfo.UTF8SaveTitle);
	std::wstring filename = Utf8ToWide(saveInfo.UTF8SaveFilename);
	std::string titleUtf8 = WideToUtf8(title);
	std::string filenameUtf8 = WideToUtf8(filename);

	char logLine[512] = {};
	sprintf_s(
		logLine,
		sizeof(logLine),
		"save[%d] title=\"%s\" filename=\"%s\"",
		index,
		titleUtf8.c_str(),
		filenameUtf8.c_str());
	LogDebug("world-io", logLine);
}

/**
 * セーブ一覧取得 (callback)
 *
 * 非同期結果を `SaveInfoQueryContext` に取り込み、待機側へ完了通知する
 */
static int GetSavesInfoCallbackProc(LPVOID lpParam, SAVE_DETAILS *pSaveDetails, const bool bRes)
{
	SaveInfoQueryContext *context = (SaveInfoQueryContext *)lpParam;
	if (context != NULL)
	{
		context->details = pSaveDetails;
		context->success = bRes;
		context->done = true;
	}
	return 0;
}

/**
 * セーブデータロード (callback)
 *
 * 破損判定などの結果を `SaveDataLoadContext` に反映する
 */
static int LoadSaveDataCallbackProc(LPVOID lpParam, const bool bIsCorrupt, const bool bIsOwner)
{
	SaveDataLoadContext *context = (SaveDataLoadContext *)lpParam;
	if (context != NULL)
	{
		context->isCorrupt = bIsCorrupt;
		context->isOwner = bIsOwner;
		context->done = true;
	}
	return 0;
}

/**
 * セーブ一覧取得の完了を待機する
 *
 * - callback 完了通知を第一候補として待つ
 * - 実装差異により callback より先に `ReturnSavesInfo()` が埋まるケースもあるため、
 *   ポーリング経路でも救済する
 *
 * @return 完了を検知できたら `true`
 */
static bool WaitForSaveInfoResult(SaveInfoQueryContext *context, DWORD timeoutMs, WorldManagerTickProc tickProc)
{
	DWORD start = GetTickCount();
	while ((GetTickCount() - start) < timeoutMs)
	{
		if (context->done)
		{
			return true;
		}

		if (context->details == NULL)
		{
			// 実装/環境によっては callback より先に ReturnSavesInfo が埋まるため、
			// callback 完了待ちだけに依存せずポーリングでも救済する
			SAVE_DETAILS *details = StorageManager.ReturnSavesInfo();
			if (details != NULL)
			{
				context->details = details;
				context->success = true;
				context->done = true;
				return true;
			}
		}

		if (tickProc != NULL)
		{
			tickProc();
		}
		Sleep(10);
	}

	return context->done;
}

/**
 * セーブ本体ロード完了 callback を待機する
 *
 * @return callback 到達で `true`、タイムアウト時は `false`
 */
static bool WaitForSaveLoadResult(SaveDataLoadContext *context, DWORD timeoutMs, WorldManagerTickProc tickProc)
{
	DWORD start = GetTickCount();
	while ((GetTickCount() - start) < timeoutMs)
	{
		if (context->done)
		{
			return true;
		}

		if (tickProc != NULL)
		{
			tickProc();
		}
		Sleep(10);
	}

	return context->done;
}

/**
 * ワールド名ベースで `SAVE_INFO` が一致するか判定する
 *
 * タイトルと保存先ファイル名の両方を比較対象にする
 */
static bool SaveInfoMatchesWorldName(const SAVE_INFO &saveInfo, const std::wstring &targetWorldName)
{
	if (targetWorldName.empty())
	{
		return false;
	}

	std::wstring saveTitle = Utf8ToWide(saveInfo.UTF8SaveTitle);
	std::wstring saveFilename = Utf8ToWide(saveInfo.UTF8SaveFilename);

	if (!saveTitle.empty() && (_wcsicmp(saveTitle.c_str(), targetWorldName.c_str()) == 0))
	{
		return true;
	}
	if (!saveFilename.empty() && (_wcsicmp(saveFilename.c_str(), targetWorldName.c_str()) == 0))
	{
		return true;
	}

	return false;
}

/**
 * 保存先ID（`UTF8SaveFilename`）で `SAVE_INFO` が一致するか判定する
 */
static bool SaveInfoMatchesSaveFilename(const SAVE_INFO &saveInfo, const std::string &targetSaveFilename)
{
	if (targetSaveFilename.empty() || saveInfo.UTF8SaveFilename[0] == 0)
	{
		return false;
	}

	return (_stricmp(saveInfo.UTF8SaveFilename, targetSaveFilename.c_str()) == 0);
}

/**
 * ワールド識別情報（`level-name` + `level-id`）をストレージ側へ適用する
 *
 * - 表示名だけ/IDだけの片設定を避け、両方を常に明示する
 * - 環境差異で新規保存先が増殖する事象を回避するための防御策
 */
static void ApplyWorldStorageTarget(const std::wstring &worldName, const std::string &saveId)
{
	// タイトル(表示名)と保存先ID(実体フォルダ名)を明示的に両方設定する
	// どちらか片方だけだと環境によって新規保存先が生成されることがある
	StorageManager.SetSaveTitle(worldName.c_str());
	SetStorageSaveUniqueFilename(saveId);
}

/**
 * 対象ワールドに一致するセーブを探索し、見つかれば起動用バイナリを抽出する
 *
 * 一致判定の優先順位:
 * 1. `level-id`（`UTF8SaveFilename`）の完全一致
 * 2. フォールバックとして `level-name` とタイトル/ファイル名一致
 *
 * @return
 * - `eWorldSaveLoad_Loaded`: 既存セーブをロードできた
 * - `eWorldSaveLoad_NotFound`: 一致セーブなし
 * - `eWorldSaveLoad_Failed`: API失敗/破損/データ不正
 */
static EWorldSaveLoadResult PrepareWorldSaveData(
	const std::wstring &targetWorldName,
	const std::string &targetSaveFilename,
	int actionPad,
	WorldManagerTickProc tickProc,
	LoadSaveDataThreadParam **outSaveData,
	std::string *outResolvedSaveFilename)
{
	if (outSaveData == NULL)
	{
		return eWorldSaveLoad_Failed;
	}
	*outSaveData = NULL;
	if (outResolvedSaveFilename != NULL)
	{
		outResolvedSaveFilename->clear();
	}

	LogWorldIO("enumerating saves for configured world");
	StorageManager.ClearSavesInfo();

	SaveInfoQueryContext infoContext;
	int infoState = StorageManager.GetSavesInfo(actionPad, &GetSavesInfoCallbackProc, &infoContext, "save");
	if (infoState == C4JStorage::ESaveGame_Idle)
	{
		infoContext.done = true;
		infoContext.success = true;
		infoContext.details = StorageManager.ReturnSavesInfo();
	}
	else if (infoState != C4JStorage::ESaveGame_GetSavesInfo)
	{
		LogWorldIO("GetSavesInfo failed to start");
		return eWorldSaveLoad_Failed;
	}

	if (!WaitForSaveInfoResult(&infoContext, 10000, tickProc))
	{
		LogWorldIO("timed out waiting for save list");
		return eWorldSaveLoad_Failed;
	}

	if (infoContext.details == NULL)
	{
		infoContext.details = StorageManager.ReturnSavesInfo();
	}
	if (infoContext.details == NULL)
	{
		LogWorldIO("failed to retrieve save list");
		return eWorldSaveLoad_Failed;
	}

	int matchedIndex = -1;
	if (!targetSaveFilename.empty())
	{
		// 1) 保存先IDが指定されている場合は最優先で一致検索
		//    これが最も安定して「同じワールド」を再利用できる(勝手に上書きで新規作成されることがある)
		for (int i = 0; i < infoContext.details->iSaveC; ++i)
		{
			LogEnumeratedSaveInfo(i, infoContext.details->SaveInfoA[i]);
			if (SaveInfoMatchesSaveFilename(infoContext.details->SaveInfoA[i], targetSaveFilename))
			{
				matchedIndex = i;
				break;
			}
		}
	}

	if (matchedIndex < 0 && targetSaveFilename.empty())
	{
		for (int i = 0; i < infoContext.details->iSaveC; ++i)
		{
			LogEnumeratedSaveInfo(i, infoContext.details->SaveInfoA[i]);
		}
	}

	for (int i = 0; i < infoContext.details->iSaveC; ++i)
	{
		// 2) 保存先IDで見つからない場合は互換フォールバックとして
		//    タイトル/ファイル名と worldName の一致を試す
		if (matchedIndex >= 0)
		{
			break;
		}
		if (SaveInfoMatchesWorldName(infoContext.details->SaveInfoA[i], targetWorldName))
		{
			matchedIndex = i;
			break;
		}
	}

	if (matchedIndex < 0)
	{
		LogWorldIO("no save matched configured world name");
		return eWorldSaveLoad_NotFound;
	}

	std::wstring matchedTitle = Utf8ToWide(infoContext.details->SaveInfoA[matchedIndex].UTF8SaveTitle);
	if (matchedTitle.empty())
	{
		matchedTitle = targetWorldName;
	}
	LogWorldName("matched save title", matchedTitle);
	SAVE_INFO *matchedSaveInfo = &infoContext.details->SaveInfoA[matchedIndex];
	std::wstring matchedFilename = Utf8ToWide(matchedSaveInfo->UTF8SaveFilename);
	if (!matchedFilename.empty())
	{
		LogWorldName("matched save filename", matchedFilename);
	}

	ApplyWorldStorageTarget(targetWorldName, targetSaveFilename);

	std::string resolvedSaveFilename;
	if (matchedSaveInfo->UTF8SaveFilename[0] != 0)
	{
		// 実際に見つかった保存先IDを優先採用し、今後の保存も同じ先に固定する
		resolvedSaveFilename = matchedSaveInfo->UTF8SaveFilename;
		SetStorageSaveUniqueFilename(resolvedSaveFilename);
	}
	else if (!targetSaveFilename.empty())
	{
		resolvedSaveFilename = targetSaveFilename;
	}

	if (outResolvedSaveFilename != NULL)
	{
		*outResolvedSaveFilename = resolvedSaveFilename;
	}

	SaveDataLoadContext loadContext;
	int loadState = StorageManager.LoadSaveData(matchedSaveInfo, &LoadSaveDataCallbackProc, &loadContext);
	if (loadState != C4JStorage::ESaveGame_Load && loadState != C4JStorage::ESaveGame_Idle)
	{
		LogWorldIO("LoadSaveData failed to start");
		return eWorldSaveLoad_Failed;
	}

	if (loadState == C4JStorage::ESaveGame_Load)
	{
		if (!WaitForSaveLoadResult(&loadContext, 15000, tickProc))
		{
			LogWorldIO("timed out waiting for save data load");
			return eWorldSaveLoad_Failed;
		}
		if (loadContext.isCorrupt)
		{
			LogWorldIO("target save is corrupt; aborting load");
			return eWorldSaveLoad_Failed;
		}
	}

	unsigned int saveSize = StorageManager.GetSaveSize();
	if (saveSize == 0)
	{
		// 読み込み成功扱いでも実データが0byteなら安全側で失敗扱いにする
		LogWorldIO("loaded save has zero size");
		return eWorldSaveLoad_Failed;
	}

	byteArray loadedSaveData(saveSize, false);
	unsigned int loadedSize = saveSize;
	StorageManager.GetSaveData(loadedSaveData.data, &loadedSize);
	if (loadedSize == 0)
	{
		LogWorldIO("failed to copy loaded save data from storage manager");
		return eWorldSaveLoad_Failed;
	}

	*outSaveData = new LoadSaveDataThreadParam(loadedSaveData.data, loadedSize, matchedTitle);
	LogWorldIO("prepared save data payload for server startup");
	return eWorldSaveLoad_Loaded;
}

/**
 * サーバー起動時のワールド状態を確定する
 *
 * - 既存セーブがあればロードして返す
 * - 見つからなければ新規ワールド文脈を準備して返す
 * - 失敗時は起動中断判断のため `Failed` を返す
 */
WorldBootstrapResult BootstrapWorldForServer(
	const ServerPropertiesConfig &config,
	int actionPad,
	WorldManagerTickProc tickProc)
{
	WorldBootstrapResult result;

	std::wstring targetWorldName = config.worldName;
	std::string targetSaveFilename = config.worldSaveId;
	if (targetWorldName.empty())
	{
		targetWorldName = L"world";
	}

	LogWorldName("configured level-name", targetWorldName);
	if (!targetSaveFilename.empty())
	{
		LogSaveFilename("configured level-id", targetSaveFilename);
	}

	ApplyWorldStorageTarget(targetWorldName, targetSaveFilename);

	std::string loadedSaveFilename;
	EWorldSaveLoadResult worldLoadResult = PrepareWorldSaveData(
		targetWorldName,
		targetSaveFilename,
		actionPad,
		tickProc,
		&result.saveData,
		&loadedSaveFilename);
	if (worldLoadResult == eWorldSaveLoad_Loaded)
	{
		result.status = eWorldBootstrap_Loaded;
		result.resolvedSaveId = loadedSaveFilename;
		LogStartupStep("loading configured world from save data");
	}
	else if (worldLoadResult == eWorldSaveLoad_NotFound)
	{
		// 一致セーブがない場合のみ新規コンテキストを作る
		// この時点で saveId を固定しておくことで、次回起動時に同じ場所へ保存される
		result.status = eWorldBootstrap_CreatedNew;
		result.resolvedSaveId = targetSaveFilename;
		LogStartupStep("configured world not found; creating new world");
		LogWorldIO("creating new world save context");
		StorageManager.ResetSaveData();
		ApplyWorldStorageTarget(targetWorldName, targetSaveFilename);
	}
	else
	{
		result.status = eWorldBootstrap_Failed;
	}

	return result;
}

/**
 * サーバー側 XUI アクションが `Idle` に戻るまで待機する
 *
 * 保存アクション中も tick/handle を継続し、非同期処理の進行停止を防ぐ
 */
bool WaitForWorldActionIdle(
	int actionPad,
	DWORD timeoutMs,
	WorldManagerTickProc tickProc,
	WorldManagerHandleActionsProc handleActionsProc)
{
	DWORD start = GetTickCount();
	while (app.GetXuiServerAction(actionPad) != eXuiServerAction_Idle && !MinecraftServer::serverHalted())
	{
		// 待機中もネットワーク/ストレージ進行を止めない
		// ここを止めると save action 自体が進まずタイムアウトしやすい
		if (tickProc != NULL)
		{
			tickProc();
		}
		if (handleActionsProc != NULL)
		{
			handleActionsProc();
		}
		if ((GetTickCount() - start) >= timeoutMs)
		{
			return false;
		}
		Sleep(10);
	}

	return (app.GetXuiServerAction(actionPad) == eXuiServerAction_Idle);
}
}


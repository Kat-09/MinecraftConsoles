#pragma once

#include <string>
#include <windows.h>

#include "ServerProperties.h"

struct _LoadSaveDataThreadParam;
typedef struct _LoadSaveDataThreadParam LoadSaveDataThreadParam;

namespace ServerRuntime
{
	/** 非同期ストレージ/ネットワーク待機中に回すティック関数 */
	typedef void (*WorldManagerTickProc)();
	/** サーバーアクション待機中に任意で回すアクション処理関数 */
	typedef void (*WorldManagerHandleActionsProc)();

	/**
	 * ワールド起動準備（既存ロード/新規作成）の結果種別
	 */
	enum EWorldBootstrapStatus
	{
		/** 既存ワールドを発見し、ロードできた */
		eWorldBootstrap_Loaded,
		/** 一致するセーブが無く、新規ワールド文脈を作成した */
		eWorldBootstrap_CreatedNew,
		/** 起動準備に失敗し、サーバー起動を中断すべき状態 */
		eWorldBootstrap_Failed
	};

	/**
	 * ワールド起動準備の出力データ
	 */
	struct WorldBootstrapResult
	{
		/** 起動準備ステータス */
		EWorldBootstrapStatus status;
		/** サーバー初期化用のセーブデータ（新規時は `NULL`） */
		LoadSaveDataThreadParam *saveData;
		/** 実際に採用された保存先ID */
		std::string resolvedSaveId;

		WorldBootstrapResult()
			: status(eWorldBootstrap_Failed)
			, saveData(NULL)
		{
		}
	};

	/**
	 * サーバー起動用に、対象ワールドのロード/新規作成を確定する
	 *
	 * - `server.properties` の `level-name` / `level-id` を適用
	 * - 既存セーブが見つかればロード
	 * - 見つからない場合のみ新規ワールドとして起動文脈を作成
	 *
	 * @param config 正規化済みの `server.properties`
	 * @param actionPad ストレージ非同期APIで使うpadId
	 * @param tickProc 非同期完了待ち中に回すティック関数
	 * @return セーブデータ有無を含む起動準備結果
	 */
	WorldBootstrapResult BootstrapWorldForServer(
		const ServerPropertiesConfig &config,
		int actionPad,
		WorldManagerTickProc tickProc);

	/**
	 * サーバーアクション状態が `Idle` へ戻るまで待機する
	 *
	 * @param actionPad 監視対象のpadId
	 * @param timeoutMs タイムアウト時間（ミリ秒）
	 * @param tickProc 待機ループ中に回すティック関数
	 * @param handleActionsProc 任意のアクション処理関数
	 * @return タイムアウト前に `Idle` 到達で `true`
	 */
	bool WaitForWorldActionIdle(
		int actionPad,
		DWORD timeoutMs,
		WorldManagerTickProc tickProc,
		WorldManagerHandleActionsProc handleActionsProc);
}

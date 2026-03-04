#pragma once

#include <string>

namespace ServerRuntime
{
	/**
	 * `server.properties`
	 */
	struct ServerPropertiesConfig
	{
		/** world name `level-name` */
		std::wstring worldName;
		/** world save id `level-id` */
		std::string worldSaveId;
	};

	/**
	 * server.properties loader
	 *
	 * - ファイル欠損時はデフォルト値で新規作成
	 * - 必須キー不足時は補完して再保存
	 * - `level-id` は保存先として安全な形式へ正規化
	 *
	 * @return `WorldManager` が利用するワールド設定
	 */
	ServerPropertiesConfig LoadServerPropertiesConfig();

	/**
	 * server.properties saver
	 *
	 * - `level-name` と `level-id` を更新
	 * - それ以外の既存キーは極力保持
	 *
	 * @param config 保存するワールド識別情報
	 * @return 書き込み成功時 `true`
	 */
	bool SaveServerPropertiesConfig(const ServerPropertiesConfig &config);
}

// BonTsCat: BonDriverから受信したTSを標準出力(またはファイル)に書き出す
// mirakcなどのチューナーコマンド(recpt1等と同じ使い方)として利用することを想定している
// ※無引数で呼び出すと"Usage:"を標準エラー出力する
//
// 引数:
// -d bondriver
//   BonDriverのファイル名またはパス。ファイル名のみのときはコマンドと同じ場所の"BonDriver"フォルダに
//   あるものを使う(EDCBと同じ)。
// -s space, range>=0, default=0
//   チューナー空間の番号。
// -c ch|space/ch
//   物理チャンネルの番号。"space/ch"の形式で空間も同時に指定できる(このとき-sは無視)。
//   区切り文字は"/"の代わりに":"か"-"も使える(mirakcのチャンネル名にはURLに含められる"-"が便利)。
// -t duration_sec|-, default=-
//   受信する秒数。"-"または0以下で無制限(標準出力が閉じられるかCtrl+C等で終了)。
// -o dest|-, default=-
//   出力ファイル名、または"-"で標準出力。
// -w open_wait_msec, range>=0, default=200
//   BonDriverのOpen後、チャンネル変更までの待ち時間。
// -i ignore_msec, range>=0, default=1000
//   チャンネル変更直後に受信したデータを捨てる時間。チャンネル変更前のパケットが来ることがあるため
//   (EDCBと同じ)。その後、最初のPATから出力する。
// -b buffer_mb, range>=1, default=64
//   出力が詰まったときに溜めておく最大量。超えた分は破棄する。
// -B b25decoder
//   スクランブル解除に使うB25Decoder互換DLLのファイル名またはパス。ファイル名のみのときはコマンドと
//   同じ場所から探す。未指定のときはスクランブル解除しない。
// -e
//   -Bのデコーダで EMM 処理を有効にする。
// -l
//   チューナー空間とチャンネルの一覧を標準出力(UTF-8)して終了する。
// -v
//   受信状況(シグナルレベルなど)を標準エラー出力する。
//
// 終了コード:
// 0: 正常終了, 1: 引数エラー, 2: BonDriverのOpen失敗, 3: チャンネル変更失敗, 4: 出力ファイルのOpen失敗,
// 5: B25Decoderのロード失敗
//
#include "stdafx.h"
#include "../../BonCtrl/BonDriverUtil.h"
#include "../../BonCtrl/ScrambleDecoderUtil.h"
#include "../../Common/PathUtil.h"
#include "../../Common/StringUtil.h"
#include "../../Common/TimeUtil.h"
#include <signal.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace
{
atomic_bool_ g_stopFlag(false);
CAutoResetEvent* g_stopEvent;

#ifdef _WIN32
BOOL WINAPI CtrlHandler(DWORD ctrlType)
{
	switch( ctrlType ){
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		g_stopFlag = true;
		g_stopEvent->Set();
		return TRUE;
	}
	return FALSE;
}
#else
void SignalHandler(int)
{
	//シグナルハンドラ内ではフラグを立てるだけ(メインループはタイムアウト付きで待っている)
	g_stopFlag = true;
}
#endif

//コンソールにはそのまま、パイプやファイルにはUTF-8で出力する
void PrintUtf8(FILE* fp, const wstring& s)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(_fileno(fp));
	DWORD mode;
	if( h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) ){
		fflush(fp);
		DWORD written;
		WriteConsoleW(h, s.c_str(), (DWORD)s.size(), &written, NULL);
		return;
	}
#endif
	string u;
	WtoUTF8(s, u);
	fputs(u.c_str(), fp);
}

void PrintUsage()
{
	fputs("Usage: BonTsCat -d bondriver [-s space] [-c ch|space/ch] [-t duration_sec|-] [-o dest|-] [-w open_wait_msec] [-i ignore_msec] [-b buffer_mb] [-B b25decoder [-e]] [-l] [-v]\n", stderr);
}

bool ParseInt(const wstring& s, int& val)
{
	if( s.empty() ){
		return false;
	}
	LPWSTR endp;
	long n = wcstol(s.c_str(), &endp, 10);
	if( *endp != L'\0' ){
		return false;
	}
	val = (int)n;
	return true;
}

//受信したTSを溜めておくバッファ
class CTsBuffer
{
public:
	CTsBuffer(size_t maxBytes_) : maxBytes(maxBytes_), dropBytes(0) {}
	void Push(const BYTE* data, DWORD size) {
		lock_recursive_mutex lock(this->bufLock);
		if( this->buf.size() + size > this->maxBytes ){
			this->dropBytes += size;
		}else{
			this->buf.insert(this->buf.end(), data, data + size);
		}
		this->event.Set();
	}
	//溜まっているデータを取り出す
	void Pop(vector<BYTE>& dest, LONGLONG& dropped) {
		dest.clear();
		lock_recursive_mutex lock(this->bufLock);
		dest.swap(this->buf);
		dropped = this->dropBytes;
		this->dropBytes = 0;
	}
	CAutoResetEvent& Event() { return this->event; }
private:
	recursive_mutex_ bufLock;
	vector<BYTE> buf;
	size_t maxBytes;
	LONGLONG dropBytes;
	CAutoResetEvent event;
};

//デコーダのDLLを直接指定してロードする
class CScrambleDecoder : public CScrambleDecoderUtil
{
public:
	using CScrambleDecoderUtil::LoadDll;
};

//DLLのパスを決める(ファイル名のみのときはコマンドと同じ場所)
wstring ResolveModulePath(const wstring& arg)
{
	fs_path path(arg);
	if( path.has_parent_path() ){
		return arg;
	}
	return GetModulePath().replace_filename(arg).native();
}

#ifdef _WIN32
//BonDriverを開けなかった理由を調べて表示する
void DiagnoseBonDriver(const wstring& path)
{
	if( UtilFileExists(path).first == false ){
		PrintUtf8(stderr, L"  原因: ファイルがありません。BonDriverフォルダに置いたか、ファイル名が正しいか確認してください。\n");
		return;
	}
	//実行ファイルのCPU種別を調べる
	std::unique_ptr<FILE, decltype(&fclose)> fp(UtilOpenFile(path, UTIL_SECURE_READ), fclose);
	BYTE header[4096] = {};
	size_t headerSize = fp ? fread(header, 1, sizeof(header), fp.get()) : 0;
	fp.reset();
	WORD machine = 0;
	if( headerSize >= 0x40 && header[0] == 'M' && header[1] == 'Z' ){
		DWORD peOffset = header[0x3C] | header[0x3D] << 8 | header[0x3E] << 16 | (DWORD)header[0x3F] << 24;
		if( peOffset + 6 <= headerSize && memcmp(header + peOffset, "PE\0\0", 4) == 0 ){
			machine = (WORD)(header[peOffset + 4] | header[peOffset + 5] << 8);
		}
	}
#ifdef _WIN64
	const WORD expectedMachine = IMAGE_FILE_MACHINE_AMD64;
	LPCWSTR expectedName = L"64bit (x64)";
#else
	const WORD expectedMachine = IMAGE_FILE_MACHINE_I386;
	LPCWSTR expectedName = L"32bit (x86)";
#endif
	if( machine != expectedMachine ){
		PrintUtf8(stderr, wstring(L"  原因: このBonDriverは") +
		                  (machine == IMAGE_FILE_MACHINE_I386 ? L"32bit (x86)" : machine == IMAGE_FILE_MACHINE_AMD64 ? L"64bit (x64)" : L"不明な形式") +
		                  L"です。" + expectedName + L"版のBonDriverが必要です。\n");
		return;
	}
	HMODULE h = LoadLibraryW(path.c_str());
	if( h == NULL ){
		DWORD err = GetLastError();
		WCHAR buff[64];
		swprintf_s(buff, L"%lu", err);
		PrintUtf8(stderr, wstring(L"  原因: DLLを読み込めません (エラー ") + buff + L")。\n");
		//読み込めない依存DLLを列挙する
		HMODULE image = LoadLibraryExW(path.c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
		if( image ){
			const BYTE* base = (const BYTE*)image;
			const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
			const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if( dir.VirtualAddress != 0 ){
				for( const IMAGE_IMPORT_DESCRIPTOR* desc = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); desc->Name != 0; desc++ ){
					LPCSTR name = (LPCSTR)(base + desc->Name);
					//APIセット(api-ms-*,ext-ms-*)はOSが解決するので調べない
					if( _strnicmp(name, "api-ms-", 7) == 0 || _strnicmp(name, "ext-ms-", 7) == 0 ){
						continue;
					}
					HMODULE dep = LoadLibraryExA(name, NULL, LOAD_LIBRARY_AS_DATAFILE);
					if( dep ){
						FreeLibrary(dep);
					}else{
						wstring wname;
						UTF8toW(name, wname);
						PrintUtf8(stderr, L"  見つからない依存DLL: " + wname + L"\n");
						if( _wcsnicmp(wname.c_str(), L"msvcp", 5) == 0 || _wcsnicmp(wname.c_str(), L"vcruntime", 9) == 0 ){
							PrintUtf8(stderr, wstring(L"    → Visual C++ 再頒布可能パッケージ (") + expectedName + L") をインストールしてください。\n");
						}
					}
				}
			}
			FreeLibrary(image);
		}
		PrintUtf8(stderr, L"  依存DLLがBonDriverと同じフォルダにある場合は、そこに置いたままで構いません。\n");
		return;
	}
	bool isBonDriver = GetProcAddress(h, "CreateBonDriver") != NULL || GetProcAddress(h, "CreateBonStruct") != NULL;
	FreeLibrary(h);
	if( isBonDriver == false ){
		PrintUtf8(stderr, L"  原因: BonDriverではないDLLです (CreateBonDriverがありません)。\n");
		return;
	}
	PrintUtf8(stderr, L"  原因: DLLは読み込めましたが、チューナーを開けませんでした (OpenTuner失敗)。\n"
	                  L"        他のソフト (TVTestなど) がチューナーを使用中でないか、ドライバやiniの設定を確認してください。\n");
}
#endif

//PAT(PID=0)のセクション先頭を含むパケットの位置を探す。見つからなければ-1
ptrdiff_t FindPatStart(const BYTE* data, size_t size)
{
	for( size_t i = 0; i + 188 <= size; i++ ){
		//同期バイトが188バイト間隔で続く位置だけを見る
		if( data[i] == 0x47 && (i + 188 * 2 > size || data[i + 188] == 0x47) ){
			for( size_t j = i; j + 188 <= size; j += 188 ){
				if( data[j] != 0x47 ){
					break;
				}
				WORD pid = (WORD)((data[j + 1] & 0x1F) << 8 | data[j + 2]);
				bool pusi = (data[j + 1] & 0x40) != 0;
				if( pid == 0 && pusi ){
					return (ptrdiff_t)j;
				}
			}
			return -1;
		}
	}
	return -1;
}

struct CFileCloser
{
	void operator()(FILE* fp) { fclose(fp); }
};

//BonDriverのフォルダとファイル名を決める
void ResolveBonDriverPath(const wstring& arg, wstring& folder, wstring& file)
{
	fs_path path(arg);
	if( path.has_parent_path() ){
		folder = path.parent_path().native();
		file = path.filename().native();
		return;
	}
	folder = GetModulePath().replace_filename(L"BonDriver").native();
	file = arg;
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
	wstring bonDriver;
	int space = 0;
	int ch = -1;
	int durationSec = 0;
	wstring destPath = L"-";
	int openWait = 200;
	int ignoreMsec = 1000;
	int bufferMB = 64;
	bool listMode = false;
	bool verbose = false;
	wstring decoderDll;
	bool emm = false;
	for( int i = 1; i < argc; i++ ){
#ifdef _WIN32
		wstring s = argv[i];
		wstring v = i + 1 < argc ? argv[i + 1] : L"";
#else
		wstring s;
		wstring v;
		UTF8toW(argv[i], s);
		if( i + 1 < argc ){
			UTF8toW(argv[i + 1], v);
		}
#endif
		bool hasValue = i + 1 < argc;
		if( s == L"-l" ){
			listMode = true;
		}else if( s == L"-v" ){
			verbose = true;
		}else if( s == L"-e" ){
			emm = true;
		}else if( hasValue && s == L"-B" ){
			decoderDll = v;
			i++;
		}else if( hasValue && s == L"-d" ){
			bonDriver = v;
			i++;
		}else if( hasValue && s == L"-s" ){
			if( ParseInt(v, space) == false || space < 0 ){
				fputs("Error: invalid space.\n", stderr);
				return 1;
			}
			i++;
		}else if( hasValue && s == L"-c" ){
			size_t sep = v.find_first_of(L"/:-");
			if( sep != wstring::npos ){
				if( ParseInt(v.substr(0, sep), space) == false || space < 0 ){
					fputs("Error: invalid space.\n", stderr);
					return 1;
				}
				v.erase(0, sep + 1);
			}
			if( ParseInt(v, ch) == false || ch < 0 ){
				fputs("Error: invalid channel.\n", stderr);
				return 1;
			}
			i++;
		}else if( hasValue && s == L"-t" ){
			if( v != L"-" && ParseInt(v, durationSec) == false ){
				fputs("Error: invalid duration.\n", stderr);
				return 1;
			}
			i++;
		}else if( hasValue && s == L"-o" ){
			destPath = v;
			i++;
		}else if( hasValue && s == L"-i" ){
			if( ParseInt(v, ignoreMsec) == false || ignoreMsec < 0 ){
				fputs("Error: invalid ignore time.\n", stderr);
				return 1;
			}
			i++;
		}else if( hasValue && s == L"-w" ){
			if( ParseInt(v, openWait) == false || openWait < 0 ){
				fputs("Error: invalid open wait.\n", stderr);
				return 1;
			}
			i++;
		}else if( hasValue && s == L"-b" ){
			if( ParseInt(v, bufferMB) == false || bufferMB < 1 ){
				fputs("Error: invalid buffer size.\n", stderr);
				return 1;
			}
			i++;
		}else{
			PrintUsage();
			return 1;
		}
	}
	if( bonDriver.empty() || (listMode == false && ch < 0) || (emm && decoderDll.empty()) ){
		PrintUsage();
		return 1;
	}

	//チューナーを開く前にデコーダを準備しておく
	CScrambleDecoder decoder;
	if( listMode == false && decoderDll.empty() == false ){
		wstring decoderPath = ResolveModulePath(decoderDll);
		if( decoder.LoadDll(decoderPath) == FALSE ){
			fputs("Error: failed to load the B25 decoder: ", stderr);
			PrintUtf8(stderr, decoderPath);
			fputs("\n", stderr);
			return 5;
		}
		if( emm ){
			//SetEmm()はロード後でないと反映されない
			decoder.SetEmm(TRUE);
		}
		if( verbose ){
			fputs("Decoder: ", stderr);
			PrintUtf8(stderr, decoderPath);
			fputs(emm ? " (EMM enabled)\n" : "\n", stderr);
		}
	}

	CAutoResetEvent stopEvent;
	g_stopEvent = &stopEvent;
#ifdef _WIN32
	SetConsoleCtrlHandler(CtrlHandler, TRUE);
#else
	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);
	signal(SIGPIPE, SIG_IGN);
#endif

	CTsBuffer tsBuff((size_t)bufferMB * 1024 * 1024);
	float signalLevel = 0;
	recursive_mutex_ statusLock;

	wstring bonFolder;
	wstring bonFile;
	ResolveBonDriverPath(bonDriver, bonFolder, bonFile);

#ifdef _WIN32
	//BonDriverが依存するDLLをBonDriverフォルダからも探せるようにする
	SetDllDirectoryW(bonFolder.c_str());
#endif

	//チャンネル変更直後のデータを捨てるための状態(ドライバのスレッドから参照する)
	atomic_bool_ ignoring(true);
	atomic_int_ chChangeTick(0);

	CBonDriverUtil bonUtil;
	if( bonUtil.OpenBonDriver(bonFolder.c_str(), bonFile.c_str(),
	                          [&](BYTE* data, DWORD size, DWORD remain) {
	                              (void)remain;
	                              if( data && size != 0 ){
	                                  if( ignoring ){
	                                      if( (int)(GetU32Tick() - (DWORD)(int)chChangeTick) < ignoreMsec ){
	                                          return;
	                                      }
	                                      ignoring = false;
	                                  }
	                                  tsBuff.Push(data, size);
	                              }
	                          },
	                          [&](float level, int, int) {
	                              lock_recursive_mutex lock(statusLock);
	                              signalLevel = level;
	                          }, 0) == false ){
		fputs("Error: failed to open the BonDriver: ", stderr);
		PrintUtf8(stderr, fs_path(bonFolder).append(bonFile).native());
		fputs("\n", stderr);
#ifdef _WIN32
		DiagnoseBonDriver(fs_path(bonFolder).append(bonFile).native());
#endif
		return 2;
	}
	if( verbose ){
		fputs("Tuner: ", stderr);
		PrintUtf8(stderr, bonUtil.GetTunerName());
		fputs("\n", stderr);
	}

	if( listMode ){
		const vector<pair<wstring, vector<wstring>>>& chList = bonUtil.GetOriginalChList();
		for( size_t i = 0; i < chList.size(); i++ ){
			for( size_t j = 0; j < chList[i].second.size(); j++ ){
				//原作に従い、チャンネル名が空のものは列挙しない
				if( chList[i].second[j].empty() == false ){
					WCHAR buff[32];
					swprintf_s(buff, L"%d-%d\t", (int)i, (int)j);
					PrintUtf8(stdout, buff + chList[i].first + L"\t" + chList[i].second[j] + L"\n");
				}
			}
		}
		bonUtil.CloseBonDriver();
		return 0;
	}

	if( openWait > 0 ){
		SleepForMsec(openWait);
	}
	chChangeTick = (int)GetU32Tick();
	ignoring = true;
	if( bonUtil.SetCh(space, ch) == false ){
		fprintf(stderr, "Error: failed to set the channel: %d/%d\n", space, ch);
		bonUtil.CloseBonDriver();
		return 3;
	}
	//チャンネル変更が終わってから数える
	chChangeTick = (int)GetU32Tick();
	{
		vector<BYTE> discard;
		LONGLONG discardDropped;
		tsBuff.Pop(discard, discardDropped);
	}
	if( verbose ){
		fprintf(stderr, "Channel: %d/%d\n", space, ch);
	}

	FILE* fp;
	std::unique_ptr<FILE, CFileCloser> destFile;
	if( destPath == L"-" ){
		fp = stdout;
#ifdef _WIN32
		_setmode(_fileno(stdout), _O_BINARY);
#endif
	}else{
		destFile.reset(UtilOpenFile(destPath, UTIL_SECURE_WRITE | UTIL_SH_READ));
		if( !destFile ){
			fputs("Error: failed to open the output file.\n", stderr);
			bonUtil.CloseBonDriver();
			return 4;
		}
		fp = destFile.get();
	}

	DWORD startTick = GetU32Tick();
	DWORD lastStatusTick = startTick;
	LONGLONG totalBytes = 0;
	vector<BYTE> data;
	//最初のPATから出力する(5秒以内に見つからなければそのまま出力する)
	bool waitPat = true;
	DWORD waitPatTick = 0;
	for( ;; ){
#ifdef _WIN32
		HANDLE handles[] = { tsBuff.Event().Handle(), stopEvent.Handle() };
		WaitForMultipleObjects(2, handles, FALSE, 1000);
#else
		tsBuff.Event().WaitOne(200);
#endif
		LONGLONG dropped;
		tsBuff.Pop(data, dropped);
		if( dropped > 0 ){
			fprintf(stderr, "Warning: dropped %lld bytes due to slow output.\n", dropped);
		}
		if( waitPat && data.empty() == false ){
			if( waitPatTick == 0 ){
				waitPatTick = GetU32Tick();
			}
			ptrdiff_t pos = FindPatStart(data.data(), data.size());
			if( pos >= 0 ){
				data.erase(data.begin(), data.begin() + pos);
				waitPat = false;
			}else if( GetU32Tick() - waitPatTick < 5000 ){
				data.clear();
			}else{
				waitPat = false;
			}
		}
		BYTE* outData = data.data();
		DWORD outSize = (DWORD)data.size();
		if( decoderDll.empty() == false && data.empty() == false ){
			if( decoder.Decode(data.data(), (DWORD)data.size(), &outData, &outSize) == FALSE ){
				//デコーダ内部で溜めている場合などは出力がないことがある
				outSize = 0;
			}
		}
		if( outSize != 0 ){
			if( fwrite(outData, 1, outSize, fp) != outSize || fflush(fp) != 0 ){
				//出力先が閉じられた
				if( verbose ){
					fputs("Output closed.\n", stderr);
				}
				break;
			}
			totalBytes += outSize;
		}
		if( g_stopFlag ){
			if( verbose ){
				fputs("Stop requested.\n", stderr);
			}
			break;
		}
		DWORD tick = GetU32Tick();
		if( durationSec > 0 && tick - startTick >= (DWORD)durationSec * 1000 ){
			break;
		}
		if( verbose && tick - lastStatusTick >= 1000 ){
			lastStatusTick = tick;
			lock_recursive_mutex lock(statusLock);
			fprintf(stderr, "Signal: %.2fdB, Received: %lld bytes\n", signalLevel, totalBytes);
		}
	}

	bonUtil.CloseBonDriver();
	return 0;
}

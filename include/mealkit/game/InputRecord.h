#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/net/Lockstep.h"

#include <cstdio>
#include <string>
#include <vector>

// 입력 기록과 틱별 해시 로그 (헤드리스·CI 결정론 비교, 3단계 리플레이의 첫 형태).
//   입력 기록(--record-inputs): 확정된 틱 입력 + 그 틱의 상태 해시. --replay-inputs로 같은 입력을 다시 돌려 해시를 비교한다.
//     빌드·OS·CPU마다 봇을 따로 돌리지 않고 같은 파일을 넣어야 빌드 사이 해시 비교가 된다.
//   해시 로그(--hash-log): 틱마다 "<틱> <해시16진>" 한 줄. 두 실행의 파일이 같으면 처음부터 끝까지 같은 상태다.
// 파일 형식(리틀 엔디언): "MKINPUT1", u32 버전, u32 인원, u64 시드, u32 매치 데이터, u32 0, u64 콘텐츠 해시,
// 그 뒤 틱마다 (인원 × 8바이트 입력, u16 명령 바이트 수 + 명령 바이트, u64 해시). 끝까지 읽은 틱이 틱 수다 (중간에 끝나도 앞부분은 쓸 수 있다).
// 버전 3은 머리 뒤에 플레이어마다 u8 로드아웃 크기 + 바이트. 버전 1(명령 없음, 틱 크기 고정)·2(로드아웃 없음)도 읽는다.
namespace mealkit::game {

// 매치를 똑같이 다시 만드는 데 필요한 값 (락스텝 세션이 정해 모두에게 보낸 것)
struct MatchInfo {
    u64 seed = 0;
    u32 playerCount = 1;
    u32 matchData = 0;
    std::vector<std::vector<u8>> loadouts; // 플레이어별 매치 시작 데이터 (mealkit/game/Loadout.h, 없으면 빈 것)
};

// 세션이 확정한 매치 값 (Running 이후)
MatchInfo matchInfo(const net::LockstepSession& session);

struct InputRecording {
    MatchInfo match;
    u64 contentHash = 0;
    std::vector<net::TickInputs> inputs; // 틱 0부터
    std::vector<u64> hashes;             // inputs[t]를 실행한 뒤의 상태 해시
};

class InputRecordWriter {
public:
    InputRecordWriter() = default;
    ~InputRecordWriter() { close(); }
    InputRecordWriter(const InputRecordWriter&) = delete;
    InputRecordWriter& operator=(const InputRecordWriter&) = delete;

    bool open(const std::string& path, const MatchInfo& match, u64 contentHash);
    bool isOpen() const { return m_file != nullptr; }
    void write(const net::TickInputs& inputs, u64 hash);
    void close();

private:
    std::FILE* m_file = nullptr;
    u32 m_players = 0;
};

bool loadInputRecording(const std::string& path, InputRecording& out, std::string& error);

// 리플레이 (매치 기록): 게임 exe·에디터 플레이가 매치마다 <사용자 폴더>/replays/current.rec에 입력 기록을 쓰고,
// 매치가 끝나면 keepReplay가 날짜·시각 이름으로 남긴다 (프로젝트 설정 keepReplays개, 오래된 것부터 지운다).
// 튕겨서 남은 current.rec도 다음 실행 때 같은 식으로 남는다 (끝까지 읽은 틱만큼 재생된다).
// 다시 보기: 게임 exe --replay <파일>, 에디터 [파일] > [리플레이 열기] (net::LockstepSession::startReplay)
inline constexpr const char* kReplayFolder = "replays";
std::string replayRecordingPath(const std::string& userFolder); // userFolder가 비어 있으면 빈 문자열 (기록하지 않는다)
// current.rec를 replays/<YYYY-MM-DD_HH-MM>.rec로 옮기고 keep개만 남긴다. 남긴 경로 (기록이 없거나 틱이 없으면 빈 문자열)
std::string keepReplay(const std::string& userFolder, u32 keep);
std::vector<std::string> listReplays(const std::string& userFolder); // 남긴 리플레이 전체 경로, 새 것부터

class HashLog {
public:
    HashLog() = default;
    ~HashLog() { close(); }
    HashLog(const HashLog&) = delete;
    HashLog& operator=(const HashLog&) = delete;

    bool open(const std::string& path);
    bool isOpen() const { return m_file != nullptr; }
    void write(u32 tick, u64 hash);
    void close();

private:
    std::FILE* m_file = nullptr;
};

} // namespace mealkit::game

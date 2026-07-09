#include "network.h"
#include "../storm/storm.h"
#include "../../Server/packet.hpp"
#include "selhero.h"

#define NO_NETWORK "No network selected. Exit game and configure connection on the front page."
#define NO_CONNECTION "Connection timed out."
#define UNKNOWN_ERROR "Connection failed."

static const char* sGetReason(RejectionReason reason) {
  switch (reason) {
  case JOIN_ALREADY_IN_GAME:
    return "Already in game";
  case JOIN_GAME_NOT_FOUND:
    return "Game not found. Check the code and server address.";
  case JOIN_INCORRECT_PASSWORD:
    return "Incorrect password";
  case JOIN_VERSION_MISMATCH:
    return "Version mismatch";
  case JOIN_GAME_FULL:
    return "Game is full";
  case CREATE_GAME_EXISTS:
    return "Game already exists";
  default:
    return "Unknown error";
  }
}

#define LVL_NIGHTMARE "Your character must reach level 20 before you can enter a multiplayer game of Nightmare difficulty."
#define LVL_HELL "Your character must reach level 30 before you can enter a multiplayer game of Hell Difficulty."

Art ArtPopupSm;
Art ArtProgBG;
Art ProgFil;
Art ButImage;

std::string g_ws_host = "127.0.0.1";

class JoinGameState : public NetworkState {
public:
  JoinGameState(const char* game, int difficulty) :
    game_(game),
    difficulty_(difficulty)
  {
    addItem({{0, 0, 640, 480}, ControlType::Image, 0, 0, "", &ArtBackground});
    textLine_ = addItem({{140, 199, 540, 310}, ControlType::Text, ControlFlags::Medium, 0, "Connecting"});
    codeLine_ = addItem({{140, 310, 540, 376}, ControlType::Text, ControlFlags::Medium | ControlFlags::Center | ControlFlags::Gold, 0, ""});
    addItem({{230, 407, 410, 449}, ControlType::Button, ControlFlags::Center | ControlFlags::Big | ControlFlags::Gold, 0, "Cancel"});
    addItem({{125, 0, 515, 154}, ControlType::Image, 0, -60, "", &ArtLogos[LOGO_MED]});
  }

  void onActivate() override {
    LoadBackgroundArt("ui_art\\black.pcx");

    if (difficulty_ < 0) {
      SNet_JoinGame(game_.c_str(), "");
    } else {
      SNet_CreateGame(game_.c_str(), "", difficulty_);
    }
  }

  void onCancel() override {
    UiPlaySelectSound();
    SNetLeaveGame(0);
    start_game(true);
  }

  void onDeactivate() override {
    if (!done_) {
      NetClose();
    }
  }

  void onClosed() override {
    activate(get_ok_dialog("Connection closed", []() {
      start_game(true);
    }));
  }

  void onInput(int value) override {
    if (value == 0) {
      onCancel();
    }
  }

  void onCodeAssign(uint32_t code, const std::string& ip) override {
    assigned_code_ = code;
    assigned_ip_ = ip;
  }

  void onJoinAccept(int player, uint64_t init_info) override {
    myplr = player;
    pfile_read_player_from_save();
    int difficulty = int(init_info >> 32);
    if (difficulty == DIFF_NIGHTMARE && plr[myplr]._pLevel < MIN_NIGHTMARE_LEVEL) {
      activate(get_ok_dialog(LVL_NIGHTMARE, []() {
        start_game(true);
      }));
      return;
    }
    if (difficulty == DIFF_HELL && plr[myplr]._pLevel < MIN_HELL_LEVEL) {
      activate(get_ok_dialog(LVL_HELL, []() {
        start_game(true);
      }));
      return;
    }
    gbLoadGame = FALSE;
    NetInit_Mid();
    postInit_ = true;
    if (NetInit_NeedSync()) {
      msg_wait_resync();
      postInit_ = true;
    } else {
      done();
    }
  }

  void done() {
    done_ = true;
    NetInit_Finish();
    activate(get_netplay_state(SELHERO_CONNECT));
  }

  void onJoinReject(int reason) override {
    activate(get_ok_dialog(sGetReason((RejectionReason) reason), []() {
      start_game(true);
    }));
  }

  void onRender(unsigned int time) override {
    std::string text;
    std::string codeText;

    if (postInit_) {
      progress_ = msg_wait_for_turns();
      text = fmtstring("Connecting (%d%%)", progress_);
    } else {
      text = "Connecting";
      for (int i = (time / 500) % 4; i > 0; --i) {
        text.push_back('.');
      }
    }

    if (difficulty_ >= 0 && assigned_code_ != 0) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Code: %06u", assigned_code_);
      codeText = buf;
      if (!assigned_ip_.empty() && assigned_ip_ != "127.0.0.1") {
        codeText += fmtstring("  IP: %s", assigned_ip_.c_str());
      }
    } else if (difficulty_ >= 0) {
      codeText = "Waiting for code...";
    }

    items[textLine_].text = text;
    items[codeLine_].text = codeText;

    NetworkState::onRender(time);
    if (postInit_ && progress_ >= 100) {
      done();
    }
  }

private:
  std::string game_;
  int difficulty_;
  int textLine_;
  int codeLine_;
  bool postInit_ = false;
  int progress_ = -1;
  bool done_ = false;
  uint32_t assigned_code_ = 0;
  std::string assigned_ip_;
};

GameStatePtr get_network_state(const char* name, const char* game, int difficulty) {
  strcpy(gszHero, name);
  pfile_create_player_description(NULL, 0);
  multi_event_handler(TRUE);
  return new JoinGameState(game, difficulty);
}

void start_multiplayer() {
  if (!SNet_HasMultiplayer()) {
    SNet_InitWebsocket();
  }
  start_game(true);
}

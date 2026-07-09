#include "../diablo.h"
#include "selhero.h"
#include "../storm/storm.h"
#include "../pfile_ex.h"
#include "../net/lan_discovery.h"
#include "network.h"

#include <thread>
#include <atomic>

#ifdef SPAWN
#define NO_CLASS "The Rogue and Sorcerer are only available in the full retail version of Diablo. For ordering information call (800) 953-SNOW."
#endif

void start_game_dialog();

GameStatePtr select_name_dialog(const _uiheroinfo& info, std::function<void(const char*)>&& next) {
  return select_string_dialog(info, gbMaxPlayers > 1 ? "New Multi Player Hero" : "New Single Player Hero", "Enter Name", std::move(next));
}

GameStatePtr select_load_dialog(const _uiheroinfo& info, std::function<void(int)>&& next) {
  return select_twoopt_dialog(&info, "Single Player Characters", "Save File Exists", "Load Game", "New Game", std::move(next));
}

// Forward declarations
void multiplayer_join_code_dialog(_uiheroinfo hero, std::string address);
void multiplayer_join_manual(_uiheroinfo hero);

// -------------------------------------------------------
// LAN Scan Dialog — scans for 2 seconds then shows list
// -------------------------------------------------------
class LanScanState : public DialogState {
public:
  explicit LanScanState(_uiheroinfo hero)
    : hero_(hero)
  {
    addItem({{0, 0, 640, 480}, ControlType::Image, 0, 0, "", &ArtBackground});
    addItem({{125, 0, 515, 154}, ControlType::Image, 0, -60, "", &ArtLogos[LOGO_MED]});
    statusLine_ = addItem({{140, 200, 500, 240}, ControlType::Text,
                           ControlFlags::Medium | ControlFlags::Center, 0, "Scanning LAN..."});

    // Up to 4 server slots (hidden initially)
    for (int i = 0; i < 4; ++i) {
      serverLines_[i] = addItem(
        {{140, (int)(260 + i * 40), 500, (int)(295 + i * 40)},
         ControlType::List, ControlFlags::Center | ControlFlags::Gold, i, ""});
    }

    addItem({{230, 425, 410, 460}, ControlType::Button,
             ControlFlags::Center | ControlFlags::Big | ControlFlags::Gold,
             -1, "Cancel"});
    wraps = false;
  }

  void onActivate() override {
    LoadBackgroundArt("ui_art\\black.pcx");
    scanThread_ = std::thread([this]() {
      servers_ = scan_lan_servers(2000);
      done_    = true;
    });
  }

  void onDeactivate() override {
    if (scanThread_.joinable()) scanThread_.join();
  }

  void onInput(int value) override {
    if (value < 0) {
      onCancel();
      return;
    }
    // value == index into servers_
    if (value >= 0 && value < (int)servers_.size()) {
      auto& srv = servers_[value];
      std::string ip = srv.ip;
      if (srv.codes.size() == 1) {
        // Only one game — go straight to connection
        char code[8];
        snprintf(code, sizeof(code), "%06u", srv.codes[0]);
        g_ws_host = ip;
        GameState::activate(get_network_state(hero_.name, code, -1));
      } else {
        // Multiple games on this server — enter code manually
        multiplayer_join_code_dialog(hero_, ip);
      }
    }
  }

  void onCancel() override {
    UiPlaySelectSound();
    start_game_dialog();
  }

  void onRender(unsigned int time) override {
    if (done_ && !processed_) {
      processed_ = true;
      if (scanThread_.joinable()) scanThread_.join();

      if (servers_.empty()) {
        items[statusLine_].text = "No servers found on LAN.";
      } else {
        items[statusLine_].text = "Select a server:";
        int n = (int)servers_.size();
        if (n > 4) n = 4;
        for (int i = 0; i < n; ++i) {
          auto& s = servers_[i];
          std::string label = s.ip + "  [";
          for (int j = 0; j < (int)s.codes.size() && j < 3; ++j) {
            if (j) label += ", ";
            char buf[8];
            snprintf(buf, sizeof(buf), "%06u", s.codes[j]);
            label += buf;
          }
          if ((int)s.codes.size() > 3) label += ", ...";
          label += "]";
          items[serverLines_[i]].text = label;
        }
        selected = 0;
      }
    }

    if (!done_) {
      std::string txt = "Scanning LAN";
      for (int i = (time / 500) % 4; i > 0; --i) txt += '.';
      items[statusLine_].text = txt;
    }

    DialogState::onRender(time);
  }

private:
  _uiheroinfo hero_;
  int statusLine_ = 0;
  int serverLines_[4] = {};
  std::thread scanThread_;
  std::vector<LanServer> servers_;
  std::atomic<bool> done_{false};
  bool processed_ = false;
};

// -------------------------------------------------------
// Multiplayer flow
// -------------------------------------------------------

void multiplayer_create_game(_uiheroinfo hero, int difficulty) {
  // Client picks a random 6-digit code; server uses it as the game key
  char code[8];
  snprintf(code, sizeof(code), "%06d", 100000 + (rand() % 900000));
  g_ws_host = "127.0.0.1";
  GameState::activate(get_network_state(hero.name, code, difficulty));
}

void multiplayer_diff_dialog(_uiheroinfo hero) {
  if (hero.level >= MIN_NIGHTMARE_LEVEL) {
    GameStatePtr prev = GameState::current();
    GameState::activate(select_diff_dialog(hero, "Create Game", [hero, prev](int value) {
      if (value < 0) {
        GameState::activate(prev);
      } else {
        multiplayer_create_game(hero, value);
      }
    }));
  } else {
    multiplayer_create_game(hero, 0);
  }
}

void multiplayer_join_code_dialog(_uiheroinfo hero, std::string address) {
  GameState::activate(select_string_dialog(hero, "Join Game", "Enter 6-Digit Code", [hero, address](const char* code) {
    if (!code) {
      start_game_dialog();
      return;
    }
    if (strlen(code) != 6) {
      GameState::activate(get_ok_dialog("Please enter exactly 6 digits.", [hero, address]() {
        multiplayer_join_code_dialog(hero, address);
      }));
      return;
    }
    for (int i = 0; i < 6; ++i) {
      if (code[i] < '0' || code[i] > '9') {
        GameState::activate(get_ok_dialog("Code must be 6 numeric digits.", [hero, address]() {
          multiplayer_join_code_dialog(hero, address);
        }));
        return;
      }
    }
    g_ws_host = address;
    GameState::activate(get_network_state(hero.name, code, -1));
  }));
}

void multiplayer_join_manual(_uiheroinfo hero) {
  GameState::activate(select_string_dialog(hero, "Join Game", "Server Address (IP)", [hero](const char* addr) {
    if (!addr) {
      start_game_dialog();
      return;
    }
    std::string address = addr;
    if (address.empty()) address = "127.0.0.1";
    multiplayer_join_code_dialog(hero, address);
  }));
}

void multiplayer_join_dialog(_uiheroinfo hero) {
  GameState::activate(select_twoopt_dialog(&hero, "Join Game", "How to connect?",
    "Scan LAN", "Manual Entry", [hero](int value) {
      if (value < 0) {
        start_game_dialog();
      } else if (value == 0) {
        GameState::activate(new LanScanState(hero));
      } else {
        multiplayer_join_manual(hero);
      }
    }));
}

void multiplayer_dialog(_uiheroinfo hero) {
  GameState::activate(select_twoopt_dialog(&hero, "Multi Player Characters", "Select Action",
    "Create Game", "Join Game", [hero](int value) {
      if (value < 0) {
        start_game_dialog();
      } else if (value == 0) {
        multiplayer_diff_dialog(hero);
      } else {
        multiplayer_join_dialog(hero);
      }
    }));
}

void name_dialog(_uiheroinfo hero) {
  GameStatePtr prev = GameState::current();
  GameState::activate(select_name_dialog(hero, [hero, prev](const char* name) mutable {
    if (name) {
      strcpy(hero.name, name);
      pfile_ui_save_create(&hero);
      if (gbMaxPlayers == 1) {
        GameState::activate(get_play_state(hero.name, SELHERO_NEW_DUNGEON));
      } else {
        multiplayer_dialog(hero);
      }
    } else {
      GameState::activate(prev);
    }
  }));
}

void create_dialog() {
  GameStatePtr prev = GameState::current();
  GameState::activate(select_class_dialog([prev](_uiheroinfo* hero) {
    if (!hero) {
      GameState::activate(prev);
    } else {
#ifdef SPAWN
      if (hero->heroclass > 0) {
        GameState::activate(get_ok_dialog(NO_CLASS, GameState::current(), false));
        return;
      }
#endif
      name_dialog(*hero);
    }
  }));
}

void newgame_dialog(_uiheroinfo hero) {
  if (hero.level >= MIN_NIGHTMARE_LEVEL) {
    GameStatePtr prev = GameState::current();
    GameState::activate(select_diff_dialog(hero, "New Game", [hero, prev](int diff) {
      if (diff < 0) {
        GameState::activate(prev);
      } else {
        NetInit_Difficulty(diff);
        GameState::activate(get_play_state(hero.name, SELHERO_NEW_DUNGEON));
      }
    }));
  } else {
    GameState::activate(get_play_state(hero.name, SELHERO_NEW_DUNGEON));
  }
}

void hero_selected_dialog(_uiheroinfo hero) {
  if (gbMaxPlayers == 1) {
    if (hero.hassaved) {
      GameStatePtr prev = GameState::current();
      GameState::activate(select_load_dialog(hero, [hero, prev](int value) {
        if (value < 0) {
          GameState::activate(prev);
        } else if (value == 0) {
          GameState::activate(get_play_state(hero.name, SELHERO_CONTINUE));
        } else if (value == 1) {
          newgame_dialog(hero);
        }
      }));
    } else {
      newgame_dialog(hero);
    }
  } else {
    multiplayer_dialog(hero);
  }
}

void start_game_dialog() {
  auto heroes = pfile_ui_set_hero_infos();
  if (heroes.empty()) {
    create_dialog();
  } else {
    GameState::activate(select_hero_dialog(heroes, [heroes](int index) {
      if (index == -1) {
        GameState::activate(get_main_menu_dialog());
      } else if (index == -2) {
        start_game_dialog();
      } else if (index >= 0 && index < (int) heroes.size()) {
        strcpy(gszHero, heroes[index].name);
        hero_selected_dialog(heroes[index]);
      } else if (index == (int) heroes.size()) {
        create_dialog();
      }
    }));
  }
}

void start_game(bool multiplayer) {
  gnDifficulty = 0;
  gbMaxPlayers = (multiplayer ? MAX_PLRS : 1);
  SNet_InitializeProvider(multiplayer);
  NetInit_Start();
  start_game_dialog();
}

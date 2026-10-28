#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

static fs::path findVideoDir() {
    std::error_code ec;

    auto config = Mod::get()->getConfigDir() / "video";
    if (fs::exists(config, ec) && fs::is_directory(config, ec)) {
        return config;
    }

    auto res = Mod::get()->getResourcesDir() / "video";
    if (fs::exists(res, ec) && fs::is_directory(res, ec)) {
        return res;
    }

    fs::create_directories(config, ec);
    return config;
}

// ---------------------------------------------------------------- video layer

class VideoLayer : public CCLayer {
protected:
    std::vector<std::string> m_frames;
    CCSprite* m_sprite = nullptr;

    fs::path m_dir;
    std::string m_lastKey;
    size_t m_index = 0;
    float m_accumulator = 0.f;
    float m_frameTime = 1.f / 30.f;
    bool m_finished = false;

    bool init() override {
        if (!CCLayer::init()) {
            return false;
        }

        auto win = CCDirector::sharedDirector()->getWinSize();
        this->setContentSize(win);

        m_dir = findVideoDir();
        if (!this->collectFrames()) {
            log::warn("no frames found in {}", m_dir.string());
            return false;
        }

        auto fps = static_cast<float>(Mod::get()->getSettingValue<double>("fps"));
        if (fps < 1.f) {
            fps = 30.f;
        }
        m_frameTime = 1.f / fps;

        m_sprite = CCSprite::create();
        m_sprite->setPosition({ win.width / 2.f, win.height / 2.f });
        m_sprite->setOpacity(0);
        this->addChild(m_sprite);

        this->setKeypadEnabled(true);
        this->showFrame(0);

        return true;
    }

    bool collectFrames() {
        std::error_code ec;
        if (!fs::exists(m_dir, ec)) {
            return false;
        }

        for (auto const& entry : fs::directory_iterator(m_dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }

            auto path = entry.path();
            auto ext = string::toLower(path.extension().string());
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
                m_frames.push_back(path.string());
            }
        }

        std::sort(m_frames.begin(), m_frames.end());
        return !m_frames.empty();
    }

    void showFrame(size_t index) {
        if (index >= m_frames.size()) {
            return;
        }

        auto cache = CCTextureCache::sharedTextureCache();
        auto const& key = m_frames[index];

        auto texture = cache->addImage(key.c_str(), false);
        if (!texture) {
            return;
        }

        auto size = texture->getContentSize();
        m_sprite->setTexture(texture);
        m_sprite->setTextureRect({ 0.f, 0.f, size.width, size.height });

        auto win = CCDirector::sharedDirector()->getWinSize();
        auto mode = Mod::get()->getSettingValue<std::string>("display-mode");

        if (mode == "window") {
            auto percent = static_cast<float>(Mod::get()->getSettingValue<double>("window-scale"));
            float scale = (win.height * percent) / size.height;

            if (size.width * scale > win.width * percent) {
                scale = (win.width * percent) / size.width;
            }

            m_sprite->setScale(scale);
        }
        else if (mode == "fit") {
            m_sprite->setScale(std::min(win.width / size.width, win.height / size.height));
        }
        else {
            m_sprite->setScaleX(win.width / size.width);
            m_sprite->setScaleY(win.height / size.height);
        }

        if (!m_lastKey.empty() && m_lastKey != key) {
            cache->removeTextureForKey(m_lastKey.c_str());
        }
        m_lastKey = key;
    }

    void startAudio() {
        auto name = Mod::get()->getSettingValue<std::string>("audio-file");
        if (name.empty()) {
            return;
        }

        std::error_code ec;
        auto path = m_dir / name;
        if (!fs::exists(path, ec)) {
            log::warn("audio file not found: {}", path.string());
            return;
        }

        FMODAudioEngine::sharedEngine()->playEffect(path.string());
    }

public:
    static VideoLayer* create() {
        auto ret = new VideoLayer();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void show() {
        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) {
            return;
        }

        scene->addChild(this, 1000);
        this->startAudio();
        this->scheduleUpdate();

        if (m_sprite) {
            m_sprite->runAction(CCFadeIn::create(0.2f));
        }
    }

    void update(float dt) override {
        if (m_finished) {
            return;
        }

        m_accumulator += dt;
        while (m_accumulator >= m_frameTime) {
            m_accumulator -= m_frameTime;
            m_index++;

            if (m_index >= m_frames.size()) {
                this->finish();
                return;
            }
        }

        this->showFrame(m_index);
    }

    void finish() {
        if (m_finished) {
            return;
        }
        m_finished = true;

        this->unscheduleUpdate();
        FMODAudioEngine::sharedEngine()->stopAllEffects();

        if (!m_lastKey.empty()) {
            CCTextureCache::sharedTextureCache()->removeTextureForKey(m_lastKey.c_str());
            m_lastKey.clear();
        }

        if (m_sprite) {
            m_sprite->runAction(CCSequence::create(
                CCFadeOut::create(0.2f),
                CCCallFunc::create(this, callfunc_selector(VideoLayer::removeSelf)),
                nullptr
            ));
        }
        else {
            this->removeSelf();
        }
    }

    void removeSelf() {
        this->removeFromParentAndCleanup(true);
    }
};

// ------------------------------------------------------------------ playlayer

class $modify(VideoPlayLayer, PlayLayer) {
    struct Fields {
        bool m_wasCompleted = false;
    };

    void levelComplete() {
        if (m_level) {
            m_fields->m_wasCompleted = m_level->m_normalPercent.value() >= 100;
        }

        PlayLayer::levelComplete();

        if (!Mod::get()->getSettingValue<bool>("enabled")) {
            return;
        }

        if (this->m_isPracticeMode || this->m_isTestMode) {
            return;
        }

        if (Mod::get()->getSettingValue<bool>("only-first-completion") && m_fields->m_wasCompleted) {
            return;
        }

        this->runAction(CCSequence::create(
            CCDelayTime::create(0.1f),
            CCCallFunc::create(this, callfunc_selector(VideoPlayLayer::showVideo)),
            nullptr
        ));
    }

    void showVideo() {
        if (auto layer = VideoLayer::create()) {
            layer->show();
        }
        else {
            log::warn("VideoLayer::create failed, skipping playback");
        }
    }
};

// ----------------------------------------------------- custom settings (v3/v4)

class CustomButtonSetting : public SettingV3 {
public:
    bool load(matjson::Value const&) override { return true; }
    bool save(matjson::Value&) const override { return true; }
    bool isDefaultValue() const override { return true; }
    void reset() override {}
};

class CustomButtonSettingNode : public SettingNodeV3 {
public:
    bool initWithButton(std::shared_ptr<SettingV3> setting, float width,
                        char const* caption, char const* texture,
                        SEL_MenuHandler handler) {
        if (!SettingNodeV3::init(setting, width)) {
            return false;
        }

        this->setContentSize({ width, 40.f });

        auto sprite = ButtonSprite::create(caption, "goldFont.fnt", texture, 0.7f);
        sprite->setScale(0.75f);

        auto button = CCMenuItemSpriteExtra::create(sprite, this, handler);

        auto menu = CCMenu::create();
        menu->addChild(button);
        menu->setPosition({ width / 2.f, 20.f });
        this->addChild(menu);

        if (auto label = this->getNameLabel()) {
            label->setVisible(false);
        }

        this->updateState(nullptr);
        return true;
    }

    void onCommit() override {}
    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
    void onResetToDefault() override {}
};

// --- скачивание пресета ---

class DownloadPresetSetting : public CustomButtonSetting {
public:
    static Result<std::shared_ptr<SettingV3>> parse(
        std::string const& key, std::string const& modID, matjson::Value const& json
    ) {
        auto res = std::make_shared<DownloadPresetSetting>();
        auto root = checkJson(json, "DownloadPresetSetting");

        res->init(key, modID, root);
        res->parseNameAndDescription(root);
        res->parseEnableIf(root);

        root.checkUnknownKeys();
        return root.ok(std::static_pointer_cast<SettingV3>(res));
    }

    SettingNodeV3* createNode(float width) override;
};
class DownloadPresetSettingNode : public CustomButtonSettingNode {
protected:
    async::TaskHolder<web::WebResponse> m_downloadTask;

    void onDownload(CCObject*) {
        static constexpr auto URL =
            "https://github.com/dontbeskid/presets/releases/download/a6/A6CAT.zip";

        FLAlertLayer::create("Downloading", "Downloading A6-CAT preset...", "OK")->show();

        // Запускаем таск через spawn с колбэком
        m_downloadTask.spawn(
            web::WebRequest().get(URL),
            [](web::WebResponse res) {
                if (!res.ok()) {
                    FLAlertLayer::create("Error", "Failed to download preset!", "OK")->show();
                    return;
                }

                auto dir = findVideoDir();
                auto zip = dir / "temp_preset.zip";

                if (!file::writeBinary(zip, res.data())) {
                    FLAlertLayer::create("Error", "Failed to save preset archive!", "OK")->show();
                    return;
                }

                auto unzipped = file::Unzip::intoDir(zip, dir);

                std::error_code ec;
                fs::remove(zip, ec);

                if (unzipped) {
                    FLAlertLayer::create("Success", "A6-CAT preset downloaded!", "OK")->show();
                }
                else {
                    FLAlertLayer::create(
                        "Error",
                        fmt::format("Failed to unzip: {}", unzipped.unwrapErr()).c_str(),
                        "OK"
                    )->show();
                }
            }
        );
    }

public:
    static DownloadPresetSettingNode* create(std::shared_ptr<DownloadPresetSetting> setting, float width) {
        auto ret = new DownloadPresetSettingNode();
        if (ret->initWithButton(
                setting, width,
                "Download A6-CAT Preset", "GJ_button_02.png",
                menu_selector(DownloadPresetSettingNode::onDownload)
            )) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

SettingNodeV3* DownloadPresetSetting::createNode(float width) {
    return DownloadPresetSettingNode::create(
        std::static_pointer_cast<DownloadPresetSetting>(shared_from_this()), width
    );
}

// --- ссылка на телеграм ---

class TelegramSetting : public CustomButtonSetting {
public:
    static Result<std::shared_ptr<SettingV3>> parse(
        std::string const& key, std::string const& modID, matjson::Value const& json
    ) {
        auto res = std::make_shared<TelegramSetting>();
        auto root = checkJson(json, "TelegramSetting");

        res->init(key, modID, root);
        res->parseNameAndDescription(root);
        res->parseEnableIf(root);

        root.checkUnknownKeys();
        return root.ok(std::static_pointer_cast<SettingV3>(res));
    }

    SettingNodeV3* createNode(float width) override;
};

class TelegramSettingNode : public CustomButtonSettingNode {
protected:
    void onTelegram(CCObject*) {
        createQuickPopup(
            "Telegram",
            "Открыть канал <cl>t.me/deepgit</c> в браузере?",
            "Отмена", "Открыть",
            [](auto, bool second) {
                if (second) {
                    web::openLinkInBrowser("https://t.me/deepgit");
                }
            }
        );
    }

public:
    static TelegramSettingNode* create(std::shared_ptr<TelegramSetting> setting, float width) {
        auto ret = new TelegramSettingNode();
        if (ret->initWithButton(
                setting, width,
                "Telegram: @deepgit", "GJ_button_01.png",
                menu_selector(TelegramSettingNode::onTelegram)
            )) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

SettingNodeV3* TelegramSetting::createNode(float width) {
    return TelegramSettingNode::create(
        std::static_pointer_cast<TelegramSetting>(shared_from_this()), width
    );
}

$execute {
    (void)Mod::get()->registerCustomSettingType("download-preset", &DownloadPresetSetting::parse);
    (void)Mod::get()->registerCustomSettingType("telegram-link", &TelegramSetting::parse);
}

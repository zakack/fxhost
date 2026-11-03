// fxhost — a small headless VST3 effect-chain host for JACK / PipeWire.
//
//   fxhost [--config PATH]          run the chain (default ~/.config/fxhost/chain.json)
//   fxhost ctl <command...>         talk to a running instance (try `fxhost ctl help`)
//
// The chain is fixed at startup. Plugin state is kept in <config dir>/state/<id>.state
// and saved on editor close, every 30 s while something changed, on `ctl save`, and on exit.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <jack/jack.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace juce;

namespace
{
std::atomic<bool> quitRequested { false };

void log (const String& s)
{
    std::cerr << s.toStdString() << std::endl;
}

String defaultSocketPath()
{
    const char* dir = std::getenv ("XDG_RUNTIME_DIR");
    return String (dir != nullptr ? dir : "/tmp") + "/fxhost.sock";
}

int connectUnix (const String& path)
{
    const int fd = ::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    path.copyToUTF8 (addr.sun_path, sizeof (addr.sun_path) - 1);

    if (::connect (fd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) != 0)
    {
        ::close (fd);
        return -1;
    }

    return fd;
}

void writeAll (int fd, const std::string& s)
{
    size_t off = 0;
    while (off < s.size())
    {
        const auto n = ::write (fd, s.data() + off, s.size() - off);
        if (n <= 0)
            return;
        off += (size_t) n;
    }
}

//==============================================================================
// VST3 class-ID handling. These hashes mirror JUCE's VST3 host so that a
// PluginDescription can be built from a class ID without scanning the bundle,
// which matters for shells like WaveShell that contain hundreds of plugins.
bool parseTuid (const String& hex, uint8 out[16])
{
    const auto h = hex.removeCharacters ("{}- ").trim();
    if (h.length() != 32 || ! h.containsOnly ("0123456789abcdefABCDEF"))
        return false;

    for (int i = 0; i < 16; ++i)
        out[i] = (uint8) h.substring (i * 2, i * 2 + 2).getHexValue32();

    return true;
}

int hashValues (const uint32* v, int n)
{
    uint32 value = 0;
    for (int i = 0; i < n; ++i)
        value = (value * 31) + v[i];
    return (int) value;
}

void fillIdsFromTuid (PluginDescription& d, const uint8 t[16])
{
    uint32 chars[16];
    for (int i = 0; i < 16; ++i)
        chars[i] = (uint32) (int) (signed char) t[i]; // TUID is char[16]; char is signed here

    uint32 longs[4];
    for (int i = 0; i < 4; ++i)
        longs[i] = ((uint32) t[i * 4] << 24) | ((uint32) t[i * 4 + 1] << 16)
                 | ((uint32) t[i * 4 + 2] << 8) | (uint32) t[i * 4 + 3];

    d.deprecatedUid = hashValues (chars, 16);
    d.uniqueId      = hashValues (longs, 4);
}

// REAPER's VST3 chunk: u32 componentSize, u32 (unknown, 1), component bytes,
// u64 controllerSize, controller bytes. Converted to JUCE's VST3PluginState.
bool convertReaperVst3Chunk (const MemoryBlock& in, MemoryBlock& out)
{
    const auto* p = static_cast<const uint8*> (in.getData());
    const auto size = in.getSize();

    if (size < 8)
        return false;

    const auto compSize = (size_t) ByteOrder::littleEndianInt (p);
    if (8 + compSize > size)
        return false;

    XmlElement state ("VST3PluginState");
    state.createNewChildElement ("IComponent")->addTextElement (MemoryBlock (p + 8, compSize).toBase64Encoding());

    const auto ctrlOff = 8 + compSize;
    if (ctrlOff + 8 <= size)
    {
        const auto ctrlSize = (size_t) ByteOrder::littleEndianInt64 (p + ctrlOff);
        if (ctrlSize > 0 && ctrlOff + 8 + ctrlSize <= size)
            state.createNewChildElement ("IEditController")
                ->addTextElement (MemoryBlock (p + ctrlOff + 8, ctrlSize).toBase64Encoding());
    }

    AudioProcessor::copyXmlToBinary (state, out);
    return true;
}

//==============================================================================
struct Slot : private AudioProcessorListener
{
    String id, name, path, uid;
    std::unique_ptr<AudioPluginInstance> plugin;
    std::atomic<bool> bypass { false };
    std::atomic<bool>* dirtyFlag = nullptr;

    int numChannels = 2, mainIn = 2, mainOut = 2;
    AudioBuffer<float> scratch;
    MidiBuffer midi;
    std::unique_ptr<DocumentWindow> window;

    ~Slot() override
    {
        if (plugin != nullptr)
            plugin->removeListener (this);
    }

    void listen (std::atomic<bool>& flag)
    {
        dirtyFlag = &flag;
        plugin->addListener (this);
    }

private:
    void audioProcessorParameterChanged (AudioProcessor*, int, float) override { *dirtyFlag = true; }
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails& d) override
    {
        if (d.parameterInfoChanged || d.programChanged || d.nonParameterStateChanged)
            *dirtyFlag = true;
    }
};

//==============================================================================
class PluginWindow final : public DocumentWindow
{
public:
    PluginWindow (Slot& s, std::function<void()> onCloseIn)
        : DocumentWindow (s.name + " - fxhost", Colours::black, DocumentWindow::closeButton),
          onClose (std::move (onCloseIn))
    {
        setUsingNativeTitleBar (true);

        AudioProcessorEditor* ed = s.plugin->hasEditor() ? s.plugin->createEditorAndMakeActive() : nullptr;
        if (ed == nullptr)
            ed = new GenericAudioProcessorEditor (*s.plugin);

        setContentOwned (ed, true);
        setResizable (ed->isResizable(), false);
        setTopLeftPosition (120, 120);
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        // The owner deletes this window, so defer.
        MessageManager::callAsync (onClose);
    }

private:
    std::function<void()> onClose;
};

//==============================================================================
class ControlServer
{
public:
    using Handler = std::function<String (const String&)>;

    ControlServer (String pathIn, Handler h) : path (std::move (pathIn)), handler (std::move (h)) {}
    ~ControlServer() { stop(); }

    bool start()
    {
        ::unlink (path.toRawUTF8());

        listenFd = ::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (listenFd < 0)
            return false;

        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        path.copyToUTF8 (addr.sun_path, sizeof (addr.sun_path) - 1);

        const auto oldMask = ::umask (0077);
        const bool ok = ::bind (listenFd, reinterpret_cast<sockaddr*> (&addr), sizeof (addr)) == 0
                     && ::listen (listenFd, 4) == 0;
        ::umask (oldMask);

        if (! ok)
        {
            ::close (listenFd);
            listenFd = -1;
            return false;
        }

        thread = std::thread ([this] { run(); });
        return true;
    }

    void stop()
    {
        stopping = true;
        if (thread.joinable())
            thread.join();
        if (listenFd >= 0)
        {
            ::close (listenFd);
            listenFd = -1;
            ::unlink (path.toRawUTF8());
        }
    }

private:
    struct Pending
    {
        String reply;
        WaitableEvent done;
    };

    void run()
    {
        while (! stopping)
        {
            pollfd pfd { listenFd, POLLIN, 0 };
            if (::poll (&pfd, 1, 200) <= 0)
                continue;

            const int fd = ::accept4 (listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0)
                continue;

            std::string line;
            char buf[512];
            for (;;)
            {
                pollfd cp { fd, POLLIN, 0 };
                if (::poll (&cp, 1, 2000) <= 0)
                    break;
                const auto n = ::read (fd, buf, sizeof (buf));
                if (n <= 0)
                    break;
                line.append (buf, (size_t) n);
                if (line.find ('\n') != std::string::npos || line.size() > 4096)
                    break;
            }

            const auto cmd = String (line).upToFirstOccurrenceOf ("\n", false, false).trim();
            auto pending = std::make_shared<Pending>();

            MessageManager::callAsync ([pending, cmd, h = handler]
            {
                pending->reply = h (cmd);
                pending->done.signal();
            });

            bool answered = false;
            for (int i = 0; i < 300 && ! stopping; ++i) // up to 30 s (plugin editors can be slow to open)
                if ((answered = pending->done.wait (100)))
                    break;

            auto reply = answered ? pending->reply : String ("error: host did not respond");
            if (! reply.endsWithChar ('\n'))
                reply << "\n";

            writeAll (fd, reply.toStdString());
            ::close (fd);
        }
    }

    String path;
    Handler handler;
    int listenFd = -1;
    std::atomic<bool> stopping { false };
    std::thread thread;
};

//==============================================================================
class FxHostApp final : public JUCEApplication, private Timer
{
public:
    const String getApplicationName() override    { return "fxhost"; }
    const String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override    { return true; } // enforced via the control socket

    void initialise (const String&) override
    {
        if (! startUp())
        {
            setApplicationReturnValue (1);
            cleanUp();
            quit();
            return;
        }

        startTimer (100);
    }

    void shutdown() override
    {
        stopTimer();
        cleanUp();
        log ("fxhost: stopped");

        // State is saved and plugins are gone. Skip static destructors: once JUCE has
        // unloaded the yabridge libraries, exit-time teardown jumps to a null pointer.
        std::cout.flush();
        std::cerr.flush();
        std::_Exit (getApplicationReturnValue());
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const String&) override {}

private:
    //==========================================================================
    bool startUp()
    {
        const auto args = getCommandLineParameterArray();
        configFile = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".config/fxhost/chain.json");

        for (int i = 0; i < args.size(); ++i)
            if ((args[i] == "--config" || args[i] == "-c") && i + 1 < args.size())
                configFile = File (args[++i]);

        const auto config = JSON::parse (configFile);
        if (! config.isObject())
        {
            log ("fxhost: cannot read config " + configFile.getFullPathName());
            return false;
        }

        clientName = config.getProperty ("client_name", "fx-chain").toString();
        maxBlock   = jlimit (64, 16384, (int) config.getProperty ("max_block", 2048));
        socketPath = config.getProperty ("socket", defaultSocketPath()).toString();
        stateDir   = configFile.getParentDirectory().getChildFile (config.getProperty ("state_dir", "state").toString());
        stateDir.createDirectory();

        if (const int fd = connectUnix (socketPath); fd >= 0)
        {
            ::close (fd);
            log ("fxhost: another instance is already running (" + socketPath + ")");
            return false;
        }

        if (auto* conn = config.getProperty ("connect", var()).getDynamicObject())
            for (auto& p : conn->getProperties())
                if (auto* arr = p.value.getArray())
                    for (auto& target : *arr)
                        connections.add ({ p.name.toString(), target.toString() });

        // JACK first, so plugins can be prepared at the real sample rate.
        jack_status_t status {};
        client = jack_client_open (clientName.toRawUTF8(), JackNoStartServer, &status);
        if (client == nullptr)
        {
            log ("fxhost: cannot open JACK client (status " + String ((int) status) + ")");
            return false;
        }

        sampleRate = (double) jack_get_sample_rate (client);
        log ("fxhost: JACK client '" + String (jack_get_client_name (client)) + "' at "
             + String (sampleRate) + " Hz, buffer " + String ((int) jack_get_buffer_size (client)));

        const char* portNames[4] = { "in_FL", "in_FR", "out_FL", "out_FR" };
        for (int i = 0; i < 4; ++i)
        {
            ports[i] = jack_port_register (client, portNames[i], JACK_DEFAULT_AUDIO_TYPE,
                                           i < 2 ? JackPortIsInput : JackPortIsOutput, 0);
            if (ports[i] == nullptr)
            {
                log ("fxhost: cannot register port " + String (portNames[i]));
                return false;
            }
        }

        addDefaultFormatsToManager (formats);

        if (auto* list = config.getProperty ("plugins", var()).getArray())
            for (auto& entry : *list)
                loadSlot (entry);

        if (slots.empty())
            log ("fxhost: warning: no plugins loaded; audio passes through unprocessed");

        mainBuffer.setSize (2, maxBlock);
        restoreBypass();

        jack_set_process_callback (client, processCallback, this);
        jack_set_sample_rate_callback (client, sampleRateCallback, this);
        jack_set_xrun_callback (client, xrunCallback, this);
        jack_set_port_registration_callback (client, portRegistrationCallback, this);
        jack_on_shutdown (client, shutdownCallback, this);

        enabled = true;
        if (jack_activate (client) != 0)
        {
            log ("fxhost: cannot activate JACK client");
            return false;
        }

        connectPorts ({});

        server = std::make_unique<ControlServer> (socketPath, [this] (const String& c) { return handleCommand (c); });
        if (! server->start())
            log ("fxhost: warning: control socket unavailable at " + socketPath);

        log ("fxhost: running " + String ((int) slots.size()) + " plugin(s); control socket " + socketPath);
        return true;
    }

    void loadSlot (const var& entry)
    {
        auto s = std::make_unique<Slot>();
        s->name = entry.getProperty ("name", "").toString();
        s->path = entry.getProperty ("path", "").toString();
        s->uid  = entry.getProperty ("uid", "").toString();
        s->id   = entry.getProperty ("id", s->name).toString();
        s->bypass = (bool) entry.getProperty ("bypass", false);

        if (s->path.isEmpty() || ! File (s->path).exists())
        {
            log ("fxhost: [" + s->id + "] plugin not found: " + s->path);
            return;
        }

        PluginDescription desc;
        bool haveDesc = false;
        uint8 tuid[16];

        if (s->name.isNotEmpty() && parseTuid (s->uid, tuid))
        {
            desc.pluginFormatName = "VST3";
            desc.fileOrIdentifier = s->path;
            desc.name = s->name;
            desc.descriptiveName = s->name;
            fillIdsFromTuid (desc, tuid);
            haveDesc = true;
        }
        else
        {
            // No class ID: scan the bundle. Fine for normal plugins, very slow for shells.
            OwnedArray<PluginDescription> found;
            for (auto* f : formats.getFormats())
                if (f->fileMightContainThisPluginType (s->path))
                    f->findAllTypesForFile (found, s->path);

            for (auto* d : found)
                if (s->name.isEmpty() || d->name.equalsIgnoreCase (s->name))
                {
                    desc = *d;
                    haveDesc = true;
                    break;
                }
        }

        if (! haveDesc)
        {
            log ("fxhost: [" + s->id + "] no plugin named '" + s->name + "' in " + s->path);
            return;
        }

        String error;
        s->plugin = formats.createPluginInstance (desc, sampleRate, maxBlock, error);
        if (s->plugin == nullptr)
        {
            log ("fxhost: [" + s->id + "] failed to load: " + error);
            return;
        }

        if (s->name.isEmpty())
            s->name = s->plugin->getName();

        // Restore state before fixing the bus layout: a state can switch sidechains back on.
        restoreState (*s);
        configureBuses (*s);

        s->plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock);
        s->plugin->prepareToPlay (sampleRate, maxBlock);
        s->scratch.setSize (maxPluginChannels, maxBlock);
        s->midi.ensureSize (1024);
        s->listen (dirty);

        log ("fxhost: [" + s->id + "] loaded '" + s->plugin->getName() + "' ("
             + String (s->mainIn) + " in / " + String (s->mainOut) + " out, latency "
             + String (s->plugin->getLatencySamples()) + ")" + (s->bypass ? " [bypassed]" : ""));

        slots.push_back (std::move (s));
    }

    static bool hasDisabledBus (const AudioProcessor& p) noexcept
    {
        for (const bool isInput : { true, false })
            for (int i = 0; i < p.getBusCount (isInput); ++i)
                if (auto* bus = p.getBus (isInput, i); bus != nullptr && ! bus->isEnabled())
                    return true;
        return false;
    }

    static void configureBuses (Slot& s)
    {
        // Keep every bus enabled. For a disabled bus JUCE passes null channel pointers,
        // which the VST3 spec allows but yabridge dereferences (segfault). Sidechains
        // therefore stay active and are fed silence in processSlot().
        auto& p = *s.plugin;
        auto layout = p.getBusesLayout();

        const auto enableAll = [&p] (Array<AudioChannelSet>& sets, bool isInput)
        {
            for (int i = 0; i < sets.size(); ++i)
                if (sets.getReference (i).isDisabled())
                {
                    auto def = p.getBus (isInput, i) != nullptr ? p.getBus (isInput, i)->getDefaultLayout() : AudioChannelSet();
                    sets.getReference (i) = def.isDisabled() ? AudioChannelSet::stereo() : def;
                }
        };
        enableAll (layout.inputBuses, true);
        enableAll (layout.outputBuses, false);

        auto stereo = layout;
        if (! stereo.inputBuses.isEmpty())  stereo.inputBuses.getReference (0)  = AudioChannelSet::stereo();
        if (! stereo.outputBuses.isEmpty()) stereo.outputBuses.getReference (0) = AudioChannelSet::stereo();

        if (! p.setBusesLayout (stereo) && ! p.setBusesLayout (layout))
            log ("fxhost: [" + s.id + "] warning: could not enable all buses");

        String desc;
        for (const bool isInput : { true, false })
            for (int i = 0; i < p.getBusCount (isInput); ++i)
                if (auto* bus = p.getBus (isInput, i))
                    desc << (isInput ? " in" : " out") << i << "=" << bus->getName() << ":"
                         << (bus->isEnabled() ? String (bus->getNumberOfChannels()) + "ch" : String ("off"));
        log ("fxhost: [" + s.id + "] buses" + desc);

        if (hasDisabledBus (p))
            log ("fxhost: [" + s.id + "] warning: a bus is still disabled; this plugin will be passed through");

        s.mainIn  = p.getMainBusNumInputChannels();
        s.mainOut = p.getMainBusNumOutputChannels();
        s.numChannels = jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    }

    //==========================================================================
    File stateFile (const Slot& s, const char* ext) const
    {
        return stateDir.getChildFile (File::createLegalFileName (s.id) + ext);
    }

    void restoreState (Slot& s)
    {
        MemoryBlock data;

        if (stateFile (s, ".state").loadFileAsData (data) && data.getSize() > 0)
        {
            s.plugin->setStateInformation (data.getData(), (int) data.getSize());
            log ("fxhost: [" + s.id + "] state restored");
        }
        else if (MemoryBlock raw; stateFile (s, ".reaper").loadFileAsData (raw) && convertReaperVst3Chunk (raw, data))
        {
            s.plugin->setStateInformation (data.getData(), (int) data.getSize());
            log ("fxhost: [" + s.id + "] state imported from REAPER chunk");
            dirty = true;
        }
    }

    void saveStates()
    {
        dirty = false;
        for (auto& s : slots)
        {
            MemoryBlock data;
            s->plugin->getStateInformation (data);
            if (data.getSize() > 0)
                stateFile (*s, ".state").replaceWithData (data.getData(), data.getSize());
        }
        saveBypass();
    }

    File bypassFile() const { return stateDir.getChildFile ("bypass.json"); }

    void saveBypass()
    {
        auto obj = std::make_unique<DynamicObject>();
        for (auto& s : slots)
            obj->setProperty (s->id, s->bypass.load());
        bypassFile().replaceWithText (JSON::toString (var (obj.release())));
    }

    void restoreBypass()
    {
        const auto v = JSON::parse (bypassFile());
        if (auto* obj = v.getDynamicObject())
            for (auto& s : slots)
                if (obj->hasProperty (s->id))
                    s->bypass = (bool) obj->getProperty (s->id);
    }

    //==========================================================================
    // Audio thread
    static int processCallback (jack_nframes_t n, void* arg) { return static_cast<FxHostApp*> (arg)->process ((int) n); }

    int process (int numFrames)
    {
        float* in[2]  = { (float*) jack_port_get_buffer (ports[0], (jack_nframes_t) numFrames),
                          (float*) jack_port_get_buffer (ports[1], (jack_nframes_t) numFrames) };
        float* out[2] = { (float*) jack_port_get_buffer (ports[2], (jack_nframes_t) numFrames),
                          (float*) jack_port_get_buffer (ports[3], (jack_nframes_t) numFrames) };

        active = true;
        if (! enabled)
        {
            active = false;
            FloatVectorOperations::clear (out[0], numFrames);
            FloatVectorOperations::clear (out[1], numFrames);
            return 0;
        }

        // Plugins were prepared for maxBlock samples; split larger periods.
        for (int pos = 0; pos < numFrames; pos += maxBlock)
        {
            const int n = jmin (maxBlock, numFrames - pos);
            FloatVectorOperations::copy (mainBuffer.getWritePointer (0), in[0] + pos, n);
            FloatVectorOperations::copy (mainBuffer.getWritePointer (1), in[1] + pos, n);

            for (auto& s : slots)
                if (! s->bypass.load (std::memory_order_relaxed))
                    processSlot (*s, n);

            FloatVectorOperations::copy (out[0] + pos, mainBuffer.getReadPointer (0), n);
            FloatVectorOperations::copy (out[1] + pos, mainBuffer.getReadPointer (1), n);
        }

        active = false;
        return 0;
    }

    void processSlot (Slot& s, int n)
    {
        const ScopedTryLock sl (s.plugin->getCallbackLock());
        if (! sl.isLocked() || s.plugin->isSuspended())
            return; // leave audio untouched this period

        // Read the layout live: a plugin may change its buses at any time (e.g. on preset load).
        auto& p = *s.plugin;
        const int numCh   = jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        const int mainIn  = p.getMainBusNumInputChannels();
        const int mainOut = p.getMainBusNumOutputChannels();

        if (numCh > s.scratch.getNumChannels() || hasDisabledBus (p))
            return; // can't feed this layout safely; pass audio through

        AudioBuffer<float> buf (s.scratch.getArrayOfWritePointers(), numCh, n);

        buf.copyFrom (0, 0, mainBuffer, 0, 0, n);
        if (mainIn >= 2)
            buf.copyFrom (1, 0, mainBuffer, 1, 0, n);
        for (int ch = jmax (1, mainIn); ch < numCh; ++ch)
            buf.clear (ch, 0, n);

        s.midi.clear();
        p.processBlock (buf, s.midi);

        if (mainOut >= 1)
        {
            mainBuffer.copyFrom (0, 0, buf, 0, 0, n);
            mainBuffer.copyFrom (1, 0, buf, mainOut >= 2 ? 1 : 0, 0, n);
        }
    }

    static int sampleRateCallback (jack_nframes_t sr, void* arg)
    {
        auto* self = static_cast<FxHostApp*> (arg);
        if ((int) sr != roundToInt (self->sampleRate))
        {
            self->enabled = false; // silence until the message thread re-prepares
            self->pendingSampleRate = (int) sr;
        }
        return 0;
    }

    static int xrunCallback (void* arg) { ++static_cast<FxHostApp*> (arg)->xruns; return 0; }

    static void portRegistrationCallback (jack_port_id_t id, int registered, void* arg)
    {
        auto* self = static_cast<FxHostApp*> (arg);
        if (! registered)
            return;
        if (auto* port = jack_port_by_id (self->client, id))
        {
            const std::lock_guard lock (self->newPortsMutex);
            self->newPorts.add (jack_port_name (port));
        }
    }

    static void shutdownCallback (void* arg) { static_cast<FxHostApp*> (arg)->jackGone = true; }

    //==========================================================================
    // Message thread
    void timerCallback() override
    {
        if (quitRequested || jackGone)
        {
            if (jackGone)
            {
                log ("fxhost: JACK server went away");
                setApplicationReturnValue (1);
            }
            stopTimer();
            quit();
            return;
        }

        if (const int sr = pendingSampleRate.exchange (0); sr > 0)
        {
            waitForAudioIdle();
            sampleRate = sr;
            for (auto& s : slots)
            {
                s->plugin->releaseResources();
                s->plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock);
                s->plugin->prepareToPlay (sampleRate, maxBlock);
            }
            enabled = true;
            log ("fxhost: sample rate changed to " + String (sr));
        }

        StringArray fresh;
        {
            const std::lock_guard lock (newPortsMutex);
            fresh.swapWith (newPorts);
        }
        if (! fresh.isEmpty())
            connectPorts (fresh);

        if (++ticks % 300 == 0 && dirty)
            saveStates();
    }

    void waitForAudioIdle()
    {
        enabled = false;
        for (int i = 0; i < 1000 && active; ++i)
            Thread::sleep (1);
    }

    // Connect configured pairs. With an empty list, connect everything; otherwise only
    // pairs that involve a newly registered port, so manual disconnections are respected.
    void connectPorts (const StringArray& onlyInvolving)
    {
        const String prefix = String (jack_get_client_name (client)) + ":";

        for (auto& [local, remote] : connections)
        {
            const auto localFull = prefix + local;
            const bool isInput = local.startsWith ("in_");
            const auto src = isInput ? remote : localFull;
            const auto dst = isInput ? localFull : remote;

            if (! onlyInvolving.isEmpty() && ! onlyInvolving.contains (src) && ! onlyInvolving.contains (dst))
                continue;

            auto* srcPort = jack_port_by_name (client, src.toRawUTF8());
            auto* dstPort = jack_port_by_name (client, dst.toRawUTF8());
            if (srcPort == nullptr || dstPort == nullptr || jack_port_connected_to (srcPort, dst.toRawUTF8()))
                continue;

            const int rc = jack_connect (client, src.toRawUTF8(), dst.toRawUTF8());
            if (rc == 0)
                log ("fxhost: connected " + src + " -> " + dst);
            else if (rc != EEXIST)
                log ("fxhost: failed to connect " + src + " -> " + dst);
        }
    }

    //==========================================================================
    Slot* findSlot (const String& ref)
    {
        if (ref.containsOnly ("0123456789") && ref.isNotEmpty())
        {
            const int i = ref.getIntValue();
            return isPositiveAndBelow (i, (int) slots.size()) ? slots[(size_t) i].get() : nullptr;
        }
        for (auto& s : slots)
            if (s->id.equalsIgnoreCase (ref))
                return s.get();
        for (auto& s : slots)
            if (s->name.containsIgnoreCase (ref))
                return s.get();
        return nullptr;
    }

    void showEditor (Slot& s)
    {
        if (s.window == nullptr)
        {
            auto* sp = &s;
            s.window = std::make_unique<PluginWindow> (s, [this, sp] { closeEditor (*sp); });
        }
        s.window->toFront (true);
    }

    void closeEditor (Slot& s)
    {
        if (s.window == nullptr)
            return;
        s.window.reset();
        saveStates();
    }

    String handleCommand (const String& line)
    {
        auto tok = StringArray::fromTokens (line, true);
        tok.removeEmptyStrings();
        const auto cmd = tok[0].toLowerCase();
        const auto rest = tok.size() > 1 ? line.fromFirstOccurrenceOf (tok[0], false, false).trim() : String();

        if (cmd.isEmpty() || cmd == "help")
            return "commands:\n"
                   "  list                      show the chain\n"
                   "  show <n|id|name>          open a plugin editor (also: show all)\n"
                   "  hide <n|id|name>          close a plugin editor (also: hide all)\n"
                   "  bypass <ref> [on|off]     toggle or set bypass\n"
                   "  save                      write plugin state to disk now\n"
                   "  status                    sample rate, buffer, DSP load, xruns\n"
                   "  reconnect                 re-create the configured connections";

        if (cmd == "list")
        {
            String r;
            for (size_t i = 0; i < slots.size(); ++i)
            {
                auto& s = *slots[i];
                r << String ((int) i).paddedLeft (' ', 2) << "  " << s.id.paddedRight (' ', 24) << "  " << s.name
                  << (s.bypass ? "  [bypassed]" : "") << (s.window != nullptr ? "  [editor open]" : "") << "\n";
            }
            return r.isEmpty() ? "(no plugins)" : r;
        }

        if (cmd == "show" || cmd == "hide")
        {
            std::vector<Slot*> targets;
            if (rest.equalsIgnoreCase ("all"))
                for (auto& s : slots) targets.push_back (s.get());
            else if (auto* s = findSlot (rest))
                targets.push_back (s);
            else
                return "error: no plugin matches '" + rest + "'";

            for (auto* s : targets)
                cmd == "show" ? showEditor (*s) : closeEditor (*s);
            return "ok";
        }

        if (cmd == "bypass")
        {
            const auto mode = tok.size() > 2 ? tok[tok.size() - 1].toLowerCase() : String();
            const auto ref = (mode == "on" || mode == "off") ? line.fromFirstOccurrenceOf (tok[0], false, false)
                                                                   .upToLastOccurrenceOf (tok[tok.size() - 1], false, false).trim()
                                                             : rest;
            auto* s = findSlot (ref);
            if (s == nullptr)
                return "error: no plugin matches '" + ref + "'";
            s->bypass = mode == "on" ? true : mode == "off" ? false : ! s->bypass.load();
            saveBypass();
            return s->name + (s->bypass ? ": bypassed" : ": active");
        }

        if (cmd == "save")
        {
            saveStates();
            return "saved to " + stateDir.getFullPathName();
        }

        if (cmd == "status")
        {
            int latency = 0;
            for (auto& s : slots)
                if (! s->bypass) latency += s->plugin->getLatencySamples();

            return "client " + String (jack_get_client_name (client)) + ", " + String (sampleRate) + " Hz, buffer "
                 + String ((int) jack_get_buffer_size (client)) + ", DSP load " + String (jack_cpu_load (client), 1)
                 + "%, xruns " + String (xruns.load()) + ", plugin latency " + String (latency) + " samples";
        }

        if (cmd == "reconnect")
        {
            connectPorts ({});
            return "ok";
        }

        return "error: unknown command '" + cmd + "' (try: help)";
    }

    //==========================================================================
    void cleanUp()
    {
        server.reset();

        if (client != nullptr)
        {
            // Make sure no process() call is in flight before touching the plugins:
            // pipewire-jack's jack_deactivate() doesn't wait for a running cycle, and
            // deactivating a plugin mid-process breaks yabridge's IPC (abort on stop).
            waitForAudioIdle();
            jack_deactivate (client);
            waitForAudioIdle();
        }

        if (! slots.empty())
        {
            for (auto& s : slots)
                s->window.reset();
            saveStates();
            for (auto& s : slots)
                s->plugin->releaseResources();
            slots.clear();
        }

        if (client != nullptr)
        {
            jack_client_close (client);
            client = nullptr;
        }
    }

    //==========================================================================
    File configFile, stateDir;
    String clientName, socketPath;
    int maxBlock = 2048;
    static constexpr int maxPluginChannels = 32;
    double sampleRate = 48000.0;

    AudioPluginFormatManager formats;
    std::vector<std::unique_ptr<Slot>> slots;
    AudioBuffer<float> mainBuffer;

    jack_client_t* client = nullptr;
    jack_port_t* ports[4] {};
    Array<std::pair<String, String>> connections;

    std::atomic<bool> enabled { false }, active { false }, jackGone { false }, dirty { false };
    std::atomic<int> pendingSampleRate { 0 }, xruns { 0 };

    std::mutex newPortsMutex;
    StringArray newPorts;

    std::unique_ptr<ControlServer> server;
    int ticks = 0;
};

//==============================================================================
int runCtl (int argc, char** argv)
{
    String cmd;
    for (int i = 2; i < argc; ++i)
        cmd << (i > 2 ? " " : "") << argv[i];

    const int fd = connectUnix (defaultSocketPath());
    if (fd < 0)
    {
        std::cerr << "fxhost is not running (no socket at " << defaultSocketPath() << ")" << std::endl;
        return 2;
    }

    writeAll (fd, (cmd.isEmpty() ? String ("help") : cmd).toStdString() + "\n");

    std::string reply;
    char buf[4096];
    ssize_t n;
    while ((n = ::read (fd, buf, sizeof (buf))) > 0)
        reply.append (buf, (size_t) n);
    ::close (fd);

    std::cout << reply << std::flush;
    return reply.rfind ("error", 0) == 0 ? 1 : 0;
}

extern "C" void onSignal (int) { quitRequested = true; }
} // namespace

JUCE_CREATE_APPLICATION_DEFINE (FxHostApp)

int main (int argc, char** argv)
{
    if (argc > 1 && std::strcmp (argv[1], "ctl") == 0)
        return runCtl (argc, argv);

    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset (&sa.sa_mask);
    for (int sig : { SIGTERM, SIGINT, SIGHUP })
        sigaction (sig, &sa, nullptr);

    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}

#include "CLAPPluginFormat.h"
#include "juce_audio_plugin_client/AU/CoreAudioUtilityClasses/CAXException.h" // CFBundleRef (there should be a better way...)
#include "juce_core/native/juce_mac_CFHelpers.h" // CFUniquePtr

#include <clap/clap.h>

namespace juce {
    namespace juce_clap_helpers {
        template<typename Range>
        static int getHashForRange(Range &&range) noexcept {
            uint32 value = 0;

            for (const auto &item: range)
                value = (value * 31) + (uint32) item;

            return (int) value;
        }

        void fillDescription (PluginDescription &description, const clap_plugin_descriptor *clapDesc)
        {
            description.manufacturerName = clapDesc->vendor;;
            description.name = clapDesc->name;
            description.descriptiveName = clapDesc->description;
            description.pluginFormatName = "CLAP";

            description.uniqueId = getHashForRange(std::string(clapDesc->id));

            description.version = clapDesc->version;
            description.category = clapDesc->features[1] != nullptr ? clapDesc->features[1]
                                                                    : clapDesc->features[0]; // @TODO: better feature detection...

            description.isInstrument = clapDesc->features[0] == std::string(CLAP_PLUGIN_FEATURE_INSTRUMENT);
        }

        static void createPluginDescription(PluginDescription &description,
                                            const File &pluginFile, const clap_plugin_descriptor *clapDesc,
                                            int numInputs, int numOutputs)
        {
            description.fileOrIdentifier = pluginFile.getFullPathName();
            description.lastFileModTime = pluginFile.getLastModificationTime();
            description.lastInfoUpdateTime = Time::getCurrentTime();

            description.numInputChannels = numInputs;
            description.numOutputChannels = numOutputs;

            fillDescription (description, clapDesc);

        }

        //==============================================================================
        struct DescriptionFactory {
            explicit DescriptionFactory(const clap_plugin_factory *pluginFactory)
                    : factory(pluginFactory) {
                jassert (pluginFactory != nullptr);
            }

            virtual ~DescriptionFactory() = default;

            Result findDescriptionsAndPerform(const File &pluginFile) {
                Result result{Result::ok()};
                const auto count = factory->get_plugin_count(factory);

                for (uint32_t i = 0; i < count; ++i) {
                    const auto clapDesc = factory->get_plugin_descriptor(factory, i);
                    PluginDescription desc;

                    createPluginDescription(desc, pluginFile, clapDesc, 2, 2); // @TODO: get inputs and outputs...

                    result = performOnDescription(desc);

                    if (result.failed())
                        break;
                }

                return result;
            }

            virtual Result performOnDescription(PluginDescription &) = 0;

        private:
            const clap_plugin_factory *factory;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DescriptionFactory)
        };

        struct DescriptionLister : public DescriptionFactory {
            explicit DescriptionLister(const clap_plugin_factory *pluginFactory)
                    : DescriptionFactory(pluginFactory) {
            }

            Result performOnDescription(PluginDescription &desc) override {
                list.add(std::make_unique<PluginDescription>(desc));
                return Result::ok();
            }

            OwnedArray<PluginDescription> list;

        private:
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DescriptionLister)
        };


        //==============================================================================
        struct DLLHandle {
            DLLHandle(const File &fileToOpen)
                    : dllFile(fileToOpen) {
                open();
                entry = reinterpret_cast<const clap_plugin_entry *> (getFunction("clap_entry"));
                jassert (entry != nullptr);

                entry->init(dllFile.getFullPathName().toStdString().c_str());
            }

            ~DLLHandle() {
#if JUCE_MAC
                if (bundleRef != nullptr)
#endif
                {
                    if (entry != nullptr)
                        entry->deinit();

//                using ExitModuleFn = bool (PLUGIN_API*) ();
//
//                if (auto* exitFn = (ExitModuleFn) getFunction (exitFnName))
//                    exitFn();

#if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD
                    library.close();
#endif
                }
            }

            //==============================================================================
            /** The factory should begin with a refCount of 1, so don't increment the reference count
                (ie: don't use a VSTComSmartPtr in here)! Its lifetime will be handled by this DLLHandle.
            */
            const clap_plugin_factory *JUCE_CALLTYPE getPluginFactory() {
                if (factory == nullptr)
                    factory = static_cast<const clap_plugin_factory *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));

                // The plugin NEEDS to provide a factory to be able to be called a VST3!
                // Most likely you are trying to load a 32-bit VST3 from a 64-bit host
                // or vice versa.
                jassert (factory != nullptr);
                return factory;
            }

            void *getFunction(const char *functionName) {
#if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD
                return library.getFunction (functionName);
#elif JUCE_MAC
                if (bundleRef == nullptr)
                    return nullptr;

                CFUniquePtr<CFStringRef> name(String(functionName).toCFString());
                return CFBundleGetFunctionPointerForName(bundleRef.get(), name.get());
#endif
            }

            File getFile() const noexcept { return dllFile; }

        private:
            File dllFile;
            const clap_plugin_entry *entry = nullptr;
            const clap_plugin_factory *factory = nullptr;

#if JUCE_WINDOWS
            static constexpr const char* entryFnName = "clap_entry";

            using EntryProc = bool (PLUGIN_API*) ();
#elif JUCE_LINUX || JUCE_BSD
            static constexpr const char* entryFnName = "clap_entry";

            using EntryProc = bool (PLUGIN_API*) (void*);
#elif JUCE_MAC
            static constexpr const char *entryFnName = "clap_entry";

            using EntryProc = bool (*)(CFBundleRef);
#endif

            //==============================================================================
#if JUCE_WINDOWS || JUCE_LINUX || JUCE_BSD
            DynamicLibrary library;

        bool open()
        {
            if (library.open (dllFile.getFullPathName()))
                return true;

            return false;
        }
#elif JUCE_MAC
            CFUniquePtr<CFBundleRef> bundleRef;

            bool open() {
                auto *utf8 = dllFile.getFullPathName().toRawUTF8();

                if (auto url = CFUniquePtr<CFURLRef>(CFURLCreateFromFileSystemRepresentation(nullptr,
                                                                                             (const UInt8 *) utf8,
                                                                                             (CFIndex) std::strlen(
                                                                                                     utf8),
                                                                                             dllFile.isDirectory()))) {
                    bundleRef.reset(CFBundleCreate(kCFAllocatorDefault, url.get()));

                    if (bundleRef != nullptr) {
                        CFObjectHolder<CFErrorRef> error;

                        if (CFBundleLoadExecutableAndReturnError(bundleRef.get(), &error.object))
                            return true;

                        if (error.object != nullptr)
                            if (auto failureMessage = CFUniquePtr<CFStringRef>(CFErrorCopyFailureReason(error.object)))
                                DBG (String::fromCFString(failureMessage.get()));

                        bundleRef = nullptr;
                    }
                }

                return false;
            }

#endif

            //==============================================================================
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DLLHandle)
        };

        struct DLLHandleCache : public DeletedAtShutdown {
            DLLHandleCache() = default;

            ~DLLHandleCache() override { clearSingletonInstance(); }

            JUCE_DECLARE_SINGLETON (DLLHandleCache, false)

            DLLHandle &findOrCreateHandle(const String &modulePath) {
#if JUCE_LINUX || JUCE_BSD
                File file (getDLLFileFromBundle (modulePath));
#else
                File file(modulePath);
#endif

                auto it = std::find_if(openHandles.begin(), openHandles.end(),
                                       [&](const std::unique_ptr<DLLHandle> &handle) {
                                           return file == handle->getFile();
                                       });

                if (it != openHandles.end())
                    return *it->get();

                openHandles.push_back(std::make_unique<DLLHandle>(file));
                return *openHandles.back().get();
            }

        private:
#if JUCE_LINUX || JUCE_BSD
            File getDLLFileFromBundle (const String& bundlePath) const
        {
            auto machineName = []() -> String
            {
                struct utsname unameData;
                auto res = uname (&unameData);

                if (res != 0)
                    return {};

                return unameData.machine;
            }();

            File file (bundlePath);

            return file.getChildFile ("Contents")
                       .getChildFile (machineName + "-linux")
                       .getChildFile (file.getFileNameWithoutExtension() + ".so");
        }
#endif

            std::vector<std::unique_ptr<DLLHandle>> openHandles;

            //==============================================================================
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DLLHandleCache)
        };


        JUCE_IMPLEMENT_SINGLETON (DLLHandleCache)
    }

    //==============================================================================
    class CLAPPluginInstance final  : public AudioPluginInstance
    {
    public:
//        //==============================================================================
//        struct VST3Parameter final  : public Parameter
//        {
//            VST3Parameter (VST3PluginInstance& parent,
//                           Steinberg::int32 vstParameterIndex,
//                           Steinberg::Vst::ParamID parameterID,
//                           bool parameterIsAutomatable)
//                    : pluginInstance (parent),
//                      vstParamIndex (vstParameterIndex),
//                      paramID (parameterID),
//                      automatable (parameterIsAutomatable)
//            {
//            }
//
//            float getValue() const override
//            {
//                return pluginInstance.cachedParamValues.get (vstParamIndex);
//            }
//
//            /*  The 'normal' setValue call, which will update both the processor and editor.
//            */
//            void setValue (float newValue) override
//            {
//                pluginInstance.cachedParamValues.set (vstParamIndex, newValue);
//            }
//
//            /*  If we're syncing the editor to the processor, the processor won't need to
//                be notified about the parameter updates, so we can avoid flagging the
//                change when updating the float cache.
//            */
//            void setValueWithoutUpdatingProcessor (float newValue)
//            {
//                pluginInstance.cachedParamValues.setWithoutNotifying (vstParamIndex, newValue);
//                sendValueChangedMessageToListeners (newValue);
//            }
//
//            String getText (float value, int maximumLength) const override
//            {
//                MessageManagerLock lock;
//
//                if (pluginInstance.editController != nullptr)
//                {
//                    Vst::String128 result;
//
//                    if (pluginInstance.editController->getParamStringByValue (paramID, value, result) == kResultOk)
//                        return toString (result).substring (0, maximumLength);
//                }
//
//                return Parameter::getText (value, maximumLength);
//            }
//
//            float getValueForText (const String& text) const override
//            {
//                MessageManagerLock lock;
//
//                if (pluginInstance.editController != nullptr)
//                {
//                    Vst::ParamValue result;
//
//                    if (pluginInstance.editController->getParamValueByString (paramID, toString (text), result) == kResultOk)
//                        return (float) result;
//                }
//
//                return Parameter::getValueForText (text);
//            }
//
//            float getDefaultValue() const override
//            {
//                return (float) getParameterInfo().defaultNormalizedValue;
//            }
//
//            String getName (int /*maximumStringLength*/) const override
//            {
//                return toString (getParameterInfo().title);
//            }
//
//            String getLabel() const override
//            {
//                return toString (getParameterInfo().units);
//            }
//
//            bool isAutomatable() const override
//            {
//                return automatable;
//            }
//
//            bool isDiscrete() const override
//            {
//                return discrete;
//            }
//
//            int getNumSteps() const override
//            {
//                return numSteps;
//            }
//
//            StringArray getAllValueStrings() const override
//            {
//                return {};
//            }
//
//            String getParameterID() const override
//            {
//                return String (paramID);
//            }
//
//            Steinberg::Vst::ParamID getParamID() const noexcept { return paramID; }
//
//        private:
//            Vst::ParameterInfo getParameterInfo() const
//            {
//                return pluginInstance.getParameterInfoForIndex (vstParamIndex);
//            }
//
//            VST3PluginInstance& pluginInstance;
//            const Steinberg::int32 vstParamIndex;
//            const Steinberg::Vst::ParamID paramID;
//            const bool automatable;
//            const int numSteps = [&]
//            {
//                auto stepCount = getParameterInfo().stepCount;
//                return stepCount == 0 ? AudioProcessor::getDefaultNumParameterSteps()
//                                      : stepCount + 1;
//            }();
//            const bool discrete = getNumSteps() != AudioProcessor::getDefaultNumParameterSteps();
//        };
//

    private:
        clap_host host;
        const clap_plugin *_plugin = nullptr;
        const clap_plugin_params *_pluginParams = nullptr;
        const clap_plugin_quick_controls *_pluginQuickControls = nullptr;
        const clap_plugin_audio_ports *_pluginAudioPorts = nullptr;
        const clap_plugin_gui *_pluginGui = nullptr;
        const clap_plugin_timer_support *_pluginTimerSupport = nullptr;
        const clap_plugin_posix_fd_support *_pluginPosixFdSupport = nullptr;
        const clap_plugin_thread_pool *_pluginThreadPool = nullptr;
        const clap_plugin_preset_load *_pluginPresetLoad = nullptr;
        const clap_plugin_state *_pluginState = nullptr;

    public:
        //==============================================================================
        explicit CLAPPluginInstance (const clap_plugin_factory* factory, const char* clapID)
                : AudioPluginInstance (BusesProperties{})
        {
            host.host_data = this;
            host.clap_version = CLAP_VERSION;
            host.name = "JUCE Clap Host";
            host.version = "0.0.1";
            host.vendor = "clap";
            host.url = "https://github.com/free-audio/clap";
//            host.get_extension = PluginHost::clapExtension;
//            host.request_callback = PluginHost::clapRequestCallback;
//            host.request_process = PluginHost::clapRequestProcess;
//            host.request_restart = PluginHost::clapRequestRestart;

            _plugin = factory->create_plugin (factory, &host, clapID);
        }

        ~CLAPPluginInstance() override
        {
            cleanup();
        }

        void cleanup()
        {
            jassert (getActiveEditor() == nullptr); // You must delete any editors before deleting the plugin instance!

            releaseResources();

            _plugin->destroy (_plugin);
        }

        //==============================================================================
        bool initialise()
        {
            // The CLAP spec mandates that initialization is called on the main thread.
            JUCE_ASSERT_MESSAGE_THREAD

            if (_plugin == nullptr)
                return false;

            if (! _plugin->init (_plugin))
                return false;

            return true;
        }

//        void getExtensions (ExtensionsVisitor& visitor) const override
//        {
//            struct Extensions :  public ExtensionsVisitor::VST3Client,
//                                 public ExtensionsVisitor::ARAClient
//            {
//                explicit Extensions (const VST3PluginInstance* instanceIn) : instance (instanceIn) {}
//
//                Steinberg::Vst::IComponent* getIComponentPtr() const noexcept override   { return instance->holder->component; }
//
//                MemoryBlock getPreset() const override             { return instance->getStateForPresetFile(); }
//
//                bool setPreset (const MemoryBlock& rawData) const override
//                {
//                    return instance->setStateFromPresetFile (rawData);
//                }
//
//                void createARAFactoryAsync (std::function<void (ARAFactoryWrapper)> cb) const noexcept override
//                {
//                    cb (ARAFactoryWrapper { ::juce::getARAFactory (*(instance->holder->module)) });
//                }
//
//                const VST3PluginInstance* instance = nullptr;
//            };
//
//            Extensions extensions { this };
//            visitor.visitVST3Client (extensions);
//
//            if (::juce::getARAFactory (*(holder->module)))
//            {
//                visitor.visitARAClient (extensions);
//            }
//        }
//
//        void* getPlatformSpecificData() override   { return holder->component; }
//
//        void updateMidiMappings()
//        {
//            // MIDI mappings will always be updated on the main thread, but we need to ensure
//            // that we're not simultaneously reading them on the audio thread.
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            if (midiMapping != nullptr)
//                storedMidiMapping.storeMappings (*midiMapping);
//        }

        //==============================================================================
        const String getName() const override
        {
            return _plugin->desc->name;
        }

//        void repopulateArrangements (Array<Vst::SpeakerArrangement>& inputArrangements, Array<Vst::SpeakerArrangement>& outputArrangements) const
//        {
//            inputArrangements.clearQuick();
//            outputArrangements.clearQuick();
//
//            auto numInputAudioBuses  = getBusCount (true);
//            auto numOutputAudioBuses = getBusCount (false);
//
//            for (int i = 0; i < numInputAudioBuses; ++i)
//                inputArrangements.add (getArrangementForBus (processor, true, i));
//
//            for (int i = 0; i < numOutputAudioBuses; ++i)
//                outputArrangements.add (getArrangementForBus (processor, false, i));
//        }
//
//        void processorLayoutsToArrangements (Array<Vst::SpeakerArrangement>& inputArrangements, Array<Vst::SpeakerArrangement>& outputArrangements)
//        {
//            inputArrangements.clearQuick();
//            outputArrangements.clearQuick();
//
//            auto numInputBuses  = getBusCount (true);
//            auto numOutputBuses = getBusCount (false);
//
//            for (int i = 0; i < numInputBuses; ++i)
//                inputArrangements.add (getVst3SpeakerArrangement (getBus (true, i)->getLastEnabledLayout()));
//
//            for (int i = 0; i < numOutputBuses; ++i)
//                outputArrangements.add (getVst3SpeakerArrangement (getBus (false, i)->getLastEnabledLayout()));
//        }

        void prepareToPlay (double newSampleRate, int estimatedSamplesPerBlock) override
        {
            // The VST3 spec requires that IComponent::setupProcessing() is called on the message
            // thread. If you call it from a different thread, some plugins may break.
            JUCE_ASSERT_MESSAGE_THREAD
            MessageManagerLock lock;

            setRateAndBufferSizeDetails (newSampleRate, estimatedSamplesPerBlock);
            _plugin->activate (_plugin, newSampleRate, 1, (uint32_t) estimatedSamplesPerBlock);
        }

        void releaseResources() override
        {
            _plugin->deactivate (_plugin);
        }

//        bool supportsDoublePrecisionProcessing() const override
//        {
//            return (processor->canProcessSampleSize (Vst::kSample64) == kResultTrue);
//        }
//
//        //==============================================================================
//        /*  Important: It is strongly recommended to use this function if you need to
//            find the JUCE parameter corresponding to a particular IEditController
//            parameter.
//            Note that a parameter at a given index in the IEditController does not
//            necessarily correspond to the parameter at the same index in
//            AudioProcessor::getParameters().
//        */
//        VST3Parameter* getParameterForID (Vst::ParamID paramID) const
//        {
//            const auto it = idToParamMap.find (paramID);
//            return it != idToParamMap.end() ? it->second : nullptr;
//        }

        //==============================================================================
        void processBlock (AudioBuffer<float>& buffer, MidiBuffer& midiMessages) override
        {
//            jassert (! isUsingDoublePrecision());
//
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            if (isActive && processor != nullptr)
//                processAudio (buffer, midiMessages, Vst::kSample32, false);
        }

        void processBlock (AudioBuffer<double>& buffer, MidiBuffer& midiMessages) override
        {
//            jassert (isUsingDoublePrecision());
//
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            if (isActive && processor != nullptr)
//                processAudio (buffer, midiMessages, Vst::kSample64, false);
        }

//        void processBlockBypassed (AudioBuffer<float>& buffer, MidiBuffer& midiMessages) override
//        {
//            jassert (! isUsingDoublePrecision());
//
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            if (bypassParam != nullptr)
//            {
//                if (isActive && processor != nullptr)
//                    processAudio (buffer, midiMessages, Vst::kSample32, true);
//            }
//            else
//            {
//                AudioProcessor::processBlockBypassed (buffer, midiMessages);
//            }
//        }
//
//        void processBlockBypassed (AudioBuffer<double>& buffer, MidiBuffer& midiMessages) override
//        {
//            jassert (isUsingDoublePrecision());
//
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            if (bypassParam != nullptr)
//            {
//                if (isActive && processor != nullptr)
//                    processAudio (buffer, midiMessages, Vst::kSample64, true);
//            }
//            else
//            {
//                AudioProcessor::processBlockBypassed (buffer, midiMessages);
//            }
//        }

        //==============================================================================
        bool canAddBus (bool) const override                                       { return false; }
        bool canRemoveBus (bool) const override                                    { return false; }

        bool isBusesLayoutSupported (const BusesLayout& layouts) const override
        {
//            const SpinLock::ScopedLockType processLock (processMutex);
//
//            // if the processor is not active, we ask the underlying plug-in if the
//            // layout is actually supported
//            if (! isActive)
//                return canApplyBusesLayout (layouts);
//
//            // not much we can do to check the layout while the audio processor is running
//            // Let's at least check if it is a VST3 compatible layout
//            for (int dir = 0; dir < 2; ++dir)
//            {
//                bool isInput = (dir == 0);
//                auto n = getBusCount (isInput);
//
//                for (int i = 0; i < n; ++i)
//                    if (getChannelLayoutOfBus (isInput, i).isDiscreteLayout())
//                        return false;
//            }

            return true;
        }

        bool canApplyBusesLayout (const BusesLayout& layouts) const override
        {
//            // someone tried to change the layout while the AudioProcessor is running
//            // call releaseResources first!
//            jassert (! isActive);
//
//            const auto previousLayout = getBusesLayout();
//            const auto result = syncBusLayouts (layouts);
//            syncBusLayouts (previousLayout);
//            return result;

            return false;
        }

        bool acceptsMidi() const override    { return false; /* hasMidiInput; */ }
        bool producesMidi() const override   { return false; /* hasMidiOutput; */ }

        //==============================================================================
        AudioProcessorParameter* getBypassParameter() const override         { return nullptr; /* bypassParam;*/ }

        //==============================================================================
        /** May return a negative value as a means of informing us that the plugin has "infinite tail," or 0 for "no tail." */
        double getTailLengthSeconds() const override
        {
//            if (processor != nullptr)
//            {
//                auto sampleRate = getSampleRate();
//
//                if (sampleRate > 0.0)
//                {
//                    auto tailSamples = processor->getTailSamples();
//
//                    if (tailSamples == Vst::kInfiniteTail)
//                        return std::numeric_limits<double>::infinity();
//
//                    return jlimit (0, 0x7fffffff, (int) processor->getTailSamples()) / sampleRate;
//                }
//            }

            return 0.0;
        }

        //==============================================================================
        AudioProcessorEditor* createEditor() override
        {
//            if (auto* view = tryCreatingView())
//                return new VST3PluginWindow (this, view);

            return nullptr;
        }

        bool hasEditor() const override
        {
            return false;
//            // (if possible, avoid creating a second instance of the editor, because that crashes some plugins)
//            if (getActiveEditor() != nullptr)
//                return true;
//
//            VSTComSmartPtr<IPlugView> view (tryCreatingView(), false);
//            return view != nullptr;
        }

        //==============================================================================
        int getNumPrograms() override                        { return 1; /* programNames.size(); */ }
        const String getProgramName (int index) override     { return {}; /* index >= 0 ? programNames[index] : String(); */ }
        void changeProgramName (int, const String&) override {}

        int getCurrentProgram() override
        {
//            if (programNames.size() > 0 && editController != nullptr)
//                if (auto* param = getParameterForID (programParameterID))
//                    return jmax (0, roundToInt (param->getValue() * (float) (programNames.size() - 1)));

            return 0;
        }

        void setCurrentProgram (int program) override
        {
//            if (programNames.size() > 0 && editController != nullptr)
//            {
//                auto value = static_cast<Vst::ParamValue> (program) / static_cast<Vst::ParamValue> (jmax (1, programNames.size() - 1));
//
//                if (auto* param = getParameterForID (programParameterID))
//                    param->setValueNotifyingHost ((float) value);
//            }
        }

//        //==============================================================================
//        void reset() override
//        {
//            const SpinLock::ScopedLockType lock (processMutex);
//
//            if (holder->component != nullptr && processor != nullptr)
//            {
//                processor->setProcessing (false);
//                holder->component->setActive (false);
//
//                holder->component->setActive (true);
//                processor->setProcessing (true);
//            }
//        }

        //==============================================================================
        void getStateInformation (MemoryBlock& destData) override
        {
//            // The VST3 plugin format requires that get/set state calls are made
//            // from the message thread.
//            // We'll lock the message manager here as a safety precaution, but some
//            // plugins may still misbehave!
//
//            JUCE_ASSERT_MESSAGE_THREAD
//            MessageManagerLock lock;
//
//            parameterDispatcher.flush();
//
//            XmlElement state ("VST3PluginState");
//
//            appendStateFrom (state, holder->component, "IComponent");
//            appendStateFrom (state, editController, "IEditController");
//
//            AudioProcessor::copyXmlToBinary (state, destData);
        }

        void setStateInformation (const void* data, int sizeInBytes) override
        {
//            // The VST3 plugin format requires that get/set state calls are made
//            // from the message thread.
//            // We'll lock the message manager here as a safety precaution, but some
//            // plugins may still misbehave!
//
//            JUCE_ASSERT_MESSAGE_THREAD
//            MessageManagerLock lock;
//
//            parameterDispatcher.flush();
//
//            if (auto head = AudioProcessor::getXmlFromBinary (data, sizeInBytes))
//            {
//                auto componentStream (createMemoryStreamForState (*head, "IComponent"));
//
//                if (componentStream != nullptr && holder->component != nullptr)
//                    holder->component->setState (componentStream);
//
//                if (editController != nullptr)
//                {
//                    if (componentStream != nullptr)
//                    {
//                        int64 result;
//                        componentStream->seek (0, IBStream::kIBSeekSet, &result);
//                        setComponentStateAndResetParameters (*componentStream);
//                    }
//
//                    auto controllerStream (createMemoryStreamForState (*head, "IEditController"));
//
//                    if (controllerStream != nullptr)
//                        editController->setState (controllerStream);
//                }
//            }
        }

//        void setComponentStateAndResetParameters (Steinberg::MemoryStream& stream)
//        {
//            jassert (editController != nullptr);
//
//            warnOnFailureIfImplemented (editController->setComponentState (&stream));
//            resetParameters();
//        }
//
//        void resetParameters()
//        {
//            for (auto* parameter : getParameters())
//            {
//                auto* vst3Param = static_cast<VST3Parameter*> (parameter);
//                const auto value = (float) editController->getParamNormalized (vst3Param->getParamID());
//                vst3Param->setValueWithoutUpdatingProcessor (value);
//            }
//        }
//
//        MemoryBlock getStateForPresetFile() const
//        {
//            VSTComSmartPtr<Steinberg::MemoryStream> memoryStream (new Steinberg::MemoryStream(), false);
//
//            if (memoryStream == nullptr || holder->component == nullptr)
//                return {};
//
//            const auto saved = Steinberg::Vst::PresetFile::savePreset (memoryStream,
//                                                                       holder->cidOfComponent,
//                                                                       holder->component,
//                                                                       editController);
//
//            if (saved)
//                return { memoryStream->getData(), static_cast<size_t> (memoryStream->getSize()) };
//
//            return {};
//        }
//
//        bool setStateFromPresetFile (const MemoryBlock& rawData) const
//        {
//            auto rawDataCopy = rawData;
//            VSTComSmartPtr<Steinberg::MemoryStream> memoryStream (new Steinberg::MemoryStream (rawDataCopy.getData(), (int) rawDataCopy.getSize()), false);
//
//            if (memoryStream == nullptr || holder->component == nullptr)
//                return false;
//
//            return Steinberg::Vst::PresetFile::loadPreset (memoryStream, holder->cidOfComponent,
//                                                           holder->component, editController, nullptr);
//        }
//
//        //==============================================================================
        void fillInPluginDescription (PluginDescription& description) const override
        {
            juce_clap_helpers::fillDescription (description, _plugin->desc);
        }

//        /** @note Not applicable to VST3 */
//        void getCurrentProgramStateInformation (MemoryBlock& destData) override
//        {
//            destData.setSize (0, true);
//        }
//
//        /** @note Not applicable to VST3 */
//        void setCurrentProgramStateInformation (const void* data, int sizeInBytes) override
//        {
//            ignoreUnused (data, sizeInBytes);
//        }
//
//    private:
//        //==============================================================================
//#if JUCE_LINUX || JUCE_BSD
//        SharedResourcePointer<RunLoop> runLoop;
//#endif
//
//        std::unique_ptr<VST3ComponentHolder> holder;
//
//        friend VST3HostContext;
//
//        // Information objects:
//        String company;
//        std::unique_ptr<PClassInfo> info;
//        std::unique_ptr<PClassInfo2> info2;
//        std::unique_ptr<PClassInfoW> infoW;
//
//        // Rudimentary interfaces:
//        VSTComSmartPtr<Vst::IEditController> editController;
//        VSTComSmartPtr<Vst::IEditController2> editController2;
//        VSTComSmartPtr<Vst::IMidiMapping> midiMapping;
//        VSTComSmartPtr<Vst::IAudioProcessor> processor;
//        VSTComSmartPtr<Vst::IComponentHandler> componentHandler;
//        VSTComSmartPtr<Vst::IComponentHandler2> componentHandler2;
//        VSTComSmartPtr<Vst::IUnitInfo> unitInfo;
//        VSTComSmartPtr<Vst::IUnitData> unitData;
//        VSTComSmartPtr<Vst::IProgramListData> programListData;
//        VSTComSmartPtr<Vst::IConnectionPoint> componentConnection, editControllerConnection;
//        VSTComSmartPtr<Vst::ChannelContext::IInfoListener> trackInfoListener;
//
//        /** The number of IO buses MUST match that of the plugin,
//            even if there aren't enough channels to process,
//            as very poorly specified by the Steinberg SDK
//        */
//        HostBufferMapper inputBusMap, outputBusMap;
//
//        StringArray programNames;
//        Vst::ParamID programParameterID = (Vst::ParamID) -1;
//
//        std::map<Vst::ParamID, VST3Parameter*> idToParamMap;
//        EditControllerParameterDispatcher parameterDispatcher;
//        StoredMidiMapping storedMidiMapping;
//
//        /*  The plugin may request a restart during playback, which may in turn
//            attempt to call functions such as setProcessing and setActive. It is an
//            error to call these functions simultaneously with
//            IAudioProcessor::process, so we use this mutex to ensure that this
//            scenario is impossible.
//        */
//        SpinLock processMutex;
//
//        //==============================================================================
//        template <typename Type>
//        static void appendStateFrom (XmlElement& head, VSTComSmartPtr<Type>& object, const String& identifier)
//        {
//            if (object != nullptr)
//            {
//                Steinberg::MemoryStream stream;
//
//                const auto result = object->getState (&stream);
//
//                if (result == kResultTrue)
//                {
//                    MemoryBlock info (stream.getData(), (size_t) stream.getSize());
//                    head.createNewChildElement (identifier)->addTextElement (info.toBase64Encoding());
//                }
//            }
//        }
//
//        static VSTComSmartPtr<Steinberg::MemoryStream> createMemoryStreamForState (XmlElement& head, StringRef identifier)
//        {
//            if (auto* state = head.getChildByName (identifier))
//            {
//                MemoryBlock mem;
//
//                if (mem.fromBase64Encoding (state->getAllSubText()))
//                {
//                    VSTComSmartPtr<Steinberg::MemoryStream> stream (new Steinberg::MemoryStream(), false);
//                    stream->setSize ((TSize) mem.getSize());
//                    mem.copyTo (stream->getData(), 0, mem.getSize());
//                    return stream;
//                }
//            }
//
//            return nullptr;
//        }
//
//        CachedParamValues cachedParamValues;
//        VSTComSmartPtr<ParameterChanges> inputParameterChanges  { new ParameterChanges };
//        VSTComSmartPtr<ParameterChanges> outputParameterChanges { new ParameterChanges };
//        VSTComSmartPtr<MidiEventList> midiInputs { new MidiEventList }, midiOutputs { new MidiEventList };
//        Vst::ProcessContext timingInfo; //< Only use this in processBlock()!
//        bool isControllerInitialised = false, isActive = false, lastProcessBlockCallWasBypass = false;
//        const bool hasMidiInput  = getNumSingleDirectionBusesFor (holder->component, MediaKind::event, Direction::input) > 0,
//                hasMidiOutput = getNumSingleDirectionBusesFor (holder->component, MediaKind::event, Direction::output) > 0;
//        VST3Parameter* bypassParam = nullptr;
//
//        //==============================================================================
//        /** Some plugins need to be "connected" to intercommunicate between their implemented classes */
//        void interconnectComponentAndController()
//        {
//            componentConnection.loadFrom (holder->component);
//            editControllerConnection.loadFrom (editController);
//
//            if (componentConnection != nullptr && editControllerConnection != nullptr)
//            {
//                warnOnFailure (componentConnection->connect (editControllerConnection));
//                warnOnFailure (editControllerConnection->connect (componentConnection));
//            }
//        }
//
//        void refreshParameterList() override
//        {
//            AudioProcessorParameterGroup newParameterTree;
//
//            // We're going to add parameter groups to the tree recursively in the same order as the
//            // first parameters contained within them.
//            std::map<Vst::UnitID, Vst::UnitInfo> infoMap;
//            std::map<Vst::UnitID, AudioProcessorParameterGroup*> groupMap;
//            groupMap[Vst::kRootUnitId] = &newParameterTree;
//
//            if (unitInfo != nullptr)
//            {
//                const auto numUnits = unitInfo->getUnitCount();
//
//                for (int i = 1; i < numUnits; ++i)
//                {
//                    Vst::UnitInfo ui{};
//                    unitInfo->getUnitInfo (i, ui);
//                    infoMap[ui.id] = std::move (ui);
//                }
//            }
//
//            {
//                auto allIds = getAllParamIDs (*editController);
//                inputParameterChanges ->initialise (allIds);
//                outputParameterChanges->initialise (allIds);
//                cachedParamValues = CachedParamValues { std::move (allIds) };
//            }
//
//            for (int i = 0; i < editController->getParameterCount(); ++i)
//            {
//                auto paramInfo = getParameterInfoForIndex (i);
//                auto* param = new VST3Parameter (*this,
//                                                 i,
//                                                 paramInfo.id,
//                                                 (paramInfo.flags & Vst::ParameterInfo::kCanAutomate) != 0);
//
//                if ((paramInfo.flags & Vst::ParameterInfo::kIsBypass) != 0)
//                    bypassParam = param;
//
//                std::function<AudioProcessorParameterGroup* (Vst::UnitID)> findOrCreateGroup;
//                findOrCreateGroup = [&groupMap, &infoMap, &findOrCreateGroup] (Vst::UnitID groupID)
//                {
//                    auto existingGroup = groupMap.find (groupID);
//
//                    if (existingGroup != groupMap.end())
//                        return existingGroup->second;
//
//                    auto groupInfo = infoMap.find (groupID);
//
//                    if (groupInfo == infoMap.end())
//                        return groupMap[Vst::kRootUnitId];
//
//                    auto* group = new AudioProcessorParameterGroup (String (groupInfo->first),
//                                                                    toString (groupInfo->second.name),
//                                                                    {});
//                    groupMap[groupInfo->first] = group;
//
//                    auto* parentGroup = findOrCreateGroup (groupInfo->second.parentUnitId);
//                    parentGroup->addChild (std::unique_ptr<AudioProcessorParameterGroup> (group));
//
//                    return group;
//                };
//
//                auto* group = findOrCreateGroup (paramInfo.unitId);
//                group->addChild (std::unique_ptr<AudioProcessorParameter> (param));
//            }
//
//            setHostedParameterTree (std::move (newParameterTree));
//
//            idToParamMap = [this]
//            {
//                std::map<Vst::ParamID, VST3Parameter*> result;
//
//                for (auto* parameter : getParameters())
//                {
//                    auto* vst3Param = static_cast<VST3Parameter*> (parameter);
//                    result.emplace (vst3Param->getParamID(), vst3Param);
//                }
//
//                return result;
//            }();
//        }
//
//        void synchroniseStates()
//        {
//            Steinberg::MemoryStream stream;
//
//            if (holder->component->getState (&stream) == kResultTrue)
//                if (stream.seek (0, Steinberg::IBStream::kIBSeekSet, nullptr) == kResultTrue)
//                    setComponentStateAndResetParameters (stream);
//        }
//
//        void grabInformationObjects()
//        {
//            processor.loadFrom (holder->component);
//            unitInfo.loadFrom (holder->component);
//            programListData.loadFrom (holder->component);
//            unitData.loadFrom (holder->component);
//            editController2.loadFrom (holder->component);
//            midiMapping.loadFrom (holder->component);
//            componentHandler.loadFrom (holder->component);
//            componentHandler2.loadFrom (holder->component);
//            trackInfoListener.loadFrom (holder->component);
//
//            if (processor == nullptr)           processor.loadFrom (editController);
//            if (unitInfo == nullptr)            unitInfo.loadFrom (editController);
//            if (programListData == nullptr)     programListData.loadFrom (editController);
//            if (unitData == nullptr)            unitData.loadFrom (editController);
//            if (editController2 == nullptr)     editController2.loadFrom (editController);
//            if (midiMapping == nullptr)         midiMapping.loadFrom (editController);
//            if (componentHandler == nullptr)    componentHandler.loadFrom (editController);
//            if (componentHandler2 == nullptr)   componentHandler2.loadFrom (editController);
//            if (trackInfoListener == nullptr)   trackInfoListener.loadFrom (editController);
//        }
//
//        void setStateForAllMidiBuses (bool newState)
//        {
//            setStateForAllEventBuses (holder->component, newState, Direction::input);
//            setStateForAllEventBuses (holder->component, newState, Direction::output);
//        }
//
//        std::vector<ChannelMapping> createChannelMappings (bool isInput) const
//        {
//            std::vector<ChannelMapping> result;
//            result.reserve ((size_t) getBusCount (isInput));
//
//            for (auto i = 0; i < getBusCount (isInput); ++i)
//                result.emplace_back (*getBus (isInput, i));
//
//            return result;
//        }
//
//        void setupIO()
//        {
//            setStateForAllMidiBuses (true);
//
//            Vst::ProcessSetup setup;
//            setup.symbolicSampleSize   = Vst::kSample32;
//            setup.maxSamplesPerBlock   = 1024;
//            setup.sampleRate           = 44100.0;
//            setup.processMode          = Vst::kRealtime;
//
//            warnOnFailure (processor->setupProcessing (setup));
//
//            inputBusMap .prepare (createChannelMappings (true));
//            outputBusMap.prepare (createChannelMappings (false));
//            setRateAndBufferSizeDetails (setup.sampleRate, (int) setup.maxSamplesPerBlock);
//        }
//
//        static AudioProcessor::BusesProperties getBusProperties (VSTComSmartPtr<Vst::IComponent>& component)
//        {
//            AudioProcessor::BusesProperties busProperties;
//            VSTComSmartPtr<Vst::IAudioProcessor> processor;
//            processor.loadFrom (component.get());
//
//            for (int dirIdx = 0; dirIdx < 2; ++dirIdx)
//            {
//                const bool isInput = (dirIdx == 0);
//                const Vst::BusDirection dir = (isInput ? Vst::kInput : Vst::kOutput);
//                const int numBuses = component->getBusCount (Vst::kAudio, dir);
//
//                for (int i = 0; i < numBuses; ++i)
//                {
//                    Vst::BusInfo info;
//
//                    if (component->getBusInfo (Vst::kAudio, dir, (Steinberg::int32) i, info) != kResultOk)
//                        continue;
//
//                    AudioChannelSet layout = (info.channelCount == 0 ? AudioChannelSet::disabled()
//                                                                     : AudioChannelSet::discreteChannels (info.channelCount));
//
//                    Vst::SpeakerArrangement arr;
//                    if (processor != nullptr && processor->getBusArrangement (dir, i, arr) == kResultOk)
//                        layout = getChannelSetForSpeakerArrangement (arr);
//
//                    busProperties.addBus (isInput, toString (info.name), layout,
//                                          (info.flags & Vst::BusInfo::kDefaultActive) != 0);
//                }
//            }
//
//            return busProperties;
//        }
//
//        //==============================================================================
//        Vst::BusInfo getBusInfo (MediaKind kind, Direction direction, int index = 0) const
//        {
//            Vst::BusInfo busInfo;
//            busInfo.mediaType = toVstType (kind);
//            busInfo.direction = toVstType (direction);
//            busInfo.channelCount = 0;
//
//            holder->component->getBusInfo (busInfo.mediaType, busInfo.direction,
//                                           (Steinberg::int32) index, busInfo);
//            return busInfo;
//        }
//
//        //==============================================================================
//        void updateBypass (bool processBlockBypassedCalled)
//        {
//            // to remain backward compatible, the logic needs to be the following:
//            // - if processBlockBypassed was called then definitely bypass the VST3
//            // - if processBlock was called then only un-bypass the VST3 if the previous
//            //   call was processBlockBypassed, otherwise do nothing
//            if (processBlockBypassedCalled)
//            {
//                if (bypassParam != nullptr && (bypassParam->getValue() == 0.0f || ! lastProcessBlockCallWasBypass))
//                    bypassParam->setValue (1.0f);
//            }
//            else
//            {
//                if (lastProcessBlockCallWasBypass && bypassParam != nullptr)
//                    bypassParam->setValue (0.0f);
//
//            }
//
//            lastProcessBlockCallWasBypass = processBlockBypassedCalled;
//        }
//
//        //==============================================================================
//        /** @note An IPlugView, when first created, should start with a ref-count of 1! */
//        IPlugView* tryCreatingView() const
//        {
//            JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED
//
//            IPlugView* v = editController->createView (Vst::ViewType::kEditor);
//
//            if (v == nullptr) v = editController->createView (nullptr);
//            if (v == nullptr) editController->queryInterface (IPlugView::iid, (void**) &v);
//
//            return v;
//        }
//
//        //==============================================================================
//        template <typename FloatType>
//        void associateWith (Vst::ProcessData& destination, AudioBuffer<FloatType>& buffer)
//        {
//            destination.inputs  = inputBusMap .getVst3LayoutForJuceBuffer (buffer);
//            destination.outputs = outputBusMap.getVst3LayoutForJuceBuffer (buffer);
//        }
//
//        void associateWith (Vst::ProcessData& destination, MidiBuffer& midiBuffer)
//        {
//            midiInputs->clear();
//            midiOutputs->clear();
//
//            if (acceptsMidi())
//            {
//                MidiEventList::hostToPluginEventList (*midiInputs,
//                                                      midiBuffer,
//                                                      storedMidiMapping,
//                                                      [this] (const auto controlID, const auto paramValue)
//                                                      {
//                                                          if (auto* param = this->getParameterForID (controlID))
//                                                              param->setValueNotifyingHost ((float) paramValue);
//                                                      });
//            }
//
//            destination.inputEvents = midiInputs;
//            destination.outputEvents = midiOutputs;
//        }
//
//        void updateTimingInformation (Vst::ProcessData& destination, double processSampleRate)
//        {
//            toProcessContext (timingInfo, getPlayHead(), processSampleRate);
//            destination.processContext = &timingInfo;
//        }
//
//        Vst::ParameterInfo getParameterInfoForIndex (Steinberg::int32 index) const
//        {
//            Vst::ParameterInfo paramInfo{};
//
//            if (editController != nullptr)
//                editController->getParameterInfo ((int32) index, paramInfo);
//
//            return paramInfo;
//        }
//
//        Vst::ProgramListInfo getProgramListInfo (int index) const
//        {
//            Vst::ProgramListInfo paramInfo{};
//
//            if (unitInfo != nullptr)
//                unitInfo->getProgramListInfo (index, paramInfo);
//
//            return paramInfo;
//        }
//
//        void syncProgramNames()
//        {
//            programNames.clear();
//
//            if (processor == nullptr || editController == nullptr)
//                return;
//
//            Vst::UnitID programUnitID;
//            Vst::ParameterInfo paramInfo{};
//
//            {
//                int idx, num = editController->getParameterCount();
//
//                for (idx = 0; idx < num; ++idx)
//                    if (editController->getParameterInfo (idx, paramInfo) == kResultOk
//                        && (paramInfo.flags & Steinberg::Vst::ParameterInfo::kIsProgramChange) != 0)
//                        break;
//
//                if (idx >= num)
//                    return;
//
//                programParameterID = paramInfo.id;
//                programUnitID = paramInfo.unitId;
//            }
//
//            if (unitInfo != nullptr)
//            {
//                Vst::UnitInfo uInfo{};
//                const int unitCount = unitInfo->getUnitCount();
//
//                for (int idx = 0; idx < unitCount; ++idx)
//                {
//                    if (unitInfo->getUnitInfo(idx, uInfo) == kResultOk
//                        && uInfo.id == programUnitID)
//                    {
//                        const int programListCount = unitInfo->getProgramListCount();
//
//                        for (int j = 0; j < programListCount; ++j)
//                        {
//                            Vst::ProgramListInfo programListInfo{};
//
//                            if (unitInfo->getProgramListInfo (j, programListInfo) == kResultOk
//                                && programListInfo.id == uInfo.programListId)
//                            {
//                                Vst::String128 name;
//
//                                for (int k = 0; k < programListInfo.programCount; ++k)
//                                    if (unitInfo->getProgramName (programListInfo.id, k, name) == kResultOk)
//                                        programNames.add (toString (name));
//
//                                return;
//                            }
//                        }
//
//                        break;
//                    }
//                }
//            }
//
//            if (editController != nullptr && paramInfo.stepCount > 0)
//            {
//                auto numPrograms = paramInfo.stepCount + 1;
//
//                for (int i = 0; i < numPrograms; ++i)
//                {
//                    auto valueNormalized = static_cast<Vst::ParamValue> (i) / static_cast<Vst::ParamValue> (paramInfo.stepCount);
//
//                    Vst::String128 programName;
//                    if (editController->getParamStringByValue (paramInfo.id, valueNormalized, programName) == kResultOk)
//                        programNames.add (toString (programName));
//                }
//            }
//        }
//
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CLAPPluginInstance)
    };

    //==============================================================================
    CLAPPluginFormat::CLAPPluginFormat()  = default;
    CLAPPluginFormat::~CLAPPluginFormat() = default;

    void CLAPPluginFormat::findAllTypesForFile (OwnedArray<PluginDescription>& results, const String& fileOrIdentifier)
    {
        if (fileMightContainThisPluginType (fileOrIdentifier))
        {
            /**
                Since there is no apparent indication if a VST3 plugin is a shell or not,
                we're stuck iterating through a VST3's factory, creating a description
                for every housed plugin.
            */

            const auto* pluginFactory (juce_clap_helpers::DLLHandleCache::getInstance()->findOrCreateHandle (fileOrIdentifier).getPluginFactory());

            if (pluginFactory != nullptr)
            {
                juce_clap_helpers::DescriptionLister lister (pluginFactory);
                lister.findDescriptionsAndPerform (File (fileOrIdentifier));
                results.addCopiesOf (lister.list);
            }
            else
            {
                jassertfalse;
            }
        }
    }

    static std::unique_ptr<AudioPluginInstance> createCLAPInstance (CLAPPluginFormat& format,
                                                                    const PluginDescription& description)
    {
        if (! format.fileMightContainThisPluginType (description.fileOrIdentifier))
            return nullptr;

        const File file { description.fileOrIdentifier };

        struct ScopedWorkingDirectory
        {
            ~ScopedWorkingDirectory() { previousWorkingDirectory.setAsCurrentWorkingDirectory(); }
            File previousWorkingDirectory = File::getCurrentWorkingDirectory();
        };

        const ScopedWorkingDirectory scope;
        file.getParentDirectory().setAsCurrentWorkingDirectory();

        const auto* pluginFactory (juce_clap_helpers::DLLHandleCache::getInstance()->findOrCreateHandle (file.getFullPathName()).getPluginFactory());
        for (uint32_t i = 0; i < pluginFactory->get_plugin_count (pluginFactory); ++i)
        {
            if (const auto* clapDesc = pluginFactory->get_plugin_descriptor (pluginFactory, i))
            {
                if (juce_clap_helpers::getHashForRange (std::string (clapDesc->id)) == description.uniqueId)
                {
                    auto pluginInstance = std::make_unique<CLAPPluginInstance>(pluginFactory, clapDesc->id);

                    if (pluginInstance->initialise())
                        return pluginInstance;
                }
            }
        }

        return {};

//        const VST3ModuleHandle::Ptr module { VST3ModuleHandle::findOrCreateModule (file, description) };
//
//        if (module == nullptr)
//            return nullptr;
//
//        auto holder = std::make_unique<VST3ComponentHolder> (module);
//
//        if (! holder->initialise())
//            return nullptr;
//
//        auto instance = std::make_unique<VST3PluginInstance> (std::move (holder));
//
//        if (! instance->initialise())
//            return nullptr;
//
//        return instance;

        return {};
    }

    void CLAPPluginFormat::createPluginInstance (const PluginDescription& description,
                                                 double, int, PluginCreationCallback callback)
    {
        auto result = createCLAPInstance (*this, description);

        const auto errorMsg = result == nullptr ? TRANS ("Unable to load XXX plug-in file").replace ("XXX", "CLAP")
                                                : String();

        callback (std::move (result), errorMsg);
    }

    bool CLAPPluginFormat::requiresUnblockedMessageThreadDuringCreation (const PluginDescription&) const
    {
        return false;
    }

    bool CLAPPluginFormat::fileMightContainThisPluginType (const String& fileOrIdentifier)
    {
        auto f = File::createFileWithoutCheckingPath (fileOrIdentifier);

        return f.hasFileExtension (".clap")
#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
               && f.exists();
#else
               && f.existsAsFile();
#endif
    }

    String CLAPPluginFormat::getNameOfPluginFromIdentifier (const String& fileOrIdentifier)
    {
        return fileOrIdentifier; //Impossible to tell because every CLAP is a type of shell...
    }

    bool CLAPPluginFormat::pluginNeedsRescanning (const PluginDescription& description)
    {
        return File (description.fileOrIdentifier).getLastModificationTime() != description.lastFileModTime;
    }

    bool CLAPPluginFormat::doesPluginStillExist (const PluginDescription& description)
    {
        return File (description.fileOrIdentifier).exists();
    }

    StringArray CLAPPluginFormat::searchPathsForPlugins (const FileSearchPath& directoriesToSearch, const bool recursive, bool)
    {
        StringArray results;

        for (int i = 0; i < directoriesToSearch.getNumPaths(); ++i)
            recursiveFileSearch (results, directoriesToSearch[i], recursive);

        return results;
    }

    void CLAPPluginFormat::recursiveFileSearch (StringArray& results, const File& directory, const bool recursive)
    {
        for (const auto& iter : RangedDirectoryIterator (directory, false, "*", File::findFilesAndDirectories))
        {
            auto f = iter.getFile();
            bool isPlugin = false;

            if (fileMightContainThisPluginType (f.getFullPathName()))
            {
                isPlugin = true;
                results.add (f.getFullPathName());
            }

            if (recursive && (! isPlugin) && f.isDirectory())
                recursiveFileSearch (results, f, true);
        }
    }

    FileSearchPath CLAPPluginFormat::getDefaultLocationsToSearch()
    {
        // @TODO: check CLAP_PATH, as documented here: https://github.com/free-audio/clap/blob/main/include/clap/entry.h

#if JUCE_WINDOWS
        const auto localAppData = File::getSpecialLocation (File::windowsLocalAppData)        .getFullPathName();
        const auto programFiles = File::getSpecialLocation (File::globalApplicationsDirectory).getFullPathName();
        return FileSearchPath { localAppData + "\\Programs\\Common\\CLAP;" + programFiles + "\\Common Files\\CLAP" };
#elif JUCE_MAC
        return FileSearchPath { "~/Library/Audio/Plug-Ins/CLAP;/Library/Audio/Plug-Ins/CLAP" };
#else
        return FileSearchPath { "~/.clap/;/usr/lib/clap/" };
#endif
    }
}

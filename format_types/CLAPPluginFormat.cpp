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

        //==============================================================================
        const String getName() const override
        {
            return _plugin->desc->name;
        }

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
        AudioProcessorParameter* getBypassParameter() const override         { return nullptr; }

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

        //==============================================================================
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

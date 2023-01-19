#include "VapourSynthPlugin.h"

#include <stdio.h>
#include <string>
#include <VapourSynth4.h>
#include <VSScript4.h>

#include "ofxsImageEffect.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

constexpr char kPluginName[] = "VapourSynth Script";
constexpr char kPluginGrouping[] = "OFX VapourSynth";
constexpr char kPluginDescription[] = "VapourSynth inline script evaluation";
constexpr char kPluginIdentifier[] = "com.vapoursynth.resolveofx";
constexpr unsigned int kPluginVersionMajor = 1;
constexpr unsigned int kPluginVersionMinor = 0;

////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class OFXVapourSynthPlugin final : public OFX::ImageEffect
{
public:
    explicit OFXVapourSynthPlugin(OfxImageEffectHandle p_Handle);
    ~OFXVapourSynthPlugin() override;

    /* Override the render */
    void render(const OFX::RenderArguments& p_Args) override;

    /* Override the getFramesNeeded */
    void getFramesNeeded(const OFX::FramesNeededArguments &pArgs, OFX::FramesNeededSetter &pFramesNeededSetter) override;

    /* Set up and run a processor */
    void setupAndProcess(const OFX::RenderArguments& p_Args);

    bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip *&identityClip, double &identityTime) override;

    void changedParam(const OFX::InstanceChangedArgs &/*args*/, const std::string &paramName) override;

private:
    bool OpenScript(const std::string &newScript, OFX::Clip *srcClip, int width, int height, int depth);

    // Does not own the following pointers
    OFX::Clip* m_dstClip;
    OFX::Clip* m_srcClip;

    const VSAPI *m_vsapi = nullptr;
    const VSSCRIPTAPI *m_vssapi = nullptr;
    VSScript *m_script = nullptr;
    VSNode *m_node = nullptr;
    std::string m_loadedScript;
    int m_width = -1;
    int m_height = -1;
    int m_depth = -1;
    std::string m_lastErrorMsg;

    OFX::IntParam *m_radiusParam;
    OFX::StringParam *m_scriptParam;
    OFX::PushButtonParam *m_errorButtonParam;
};

OFXVapourSynthPlugin::OFXVapourSynthPlugin(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
{
    m_dstClip = fetchClip(kOfxImageEffectOutputClipName);
    m_srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    m_radiusParam = fetchIntParam("radius");
    m_scriptParam = fetchStringParam("script");
    m_errorButtonParam = fetchPushButtonParam("errorbutton");

    m_vssapi = getVSScriptAPI(VSSCRIPT_API_VERSION);
    m_vsapi = m_vssapi->getVSAPI(VAPOURSYNTH_API_VERSION);
}

OFXVapourSynthPlugin::~OFXVapourSynthPlugin() {
    m_vsapi->freeNode(m_node);
    m_vssapi->freeScript(m_script);
}

// If the plugin wants change the frames needed on an input clip from the default values (which is the same as the frame to be renderred),
// it should do so by calling the OFX::FramesNeededSetter::setFramesNeeded function with the desired frame range.
void OFXVapourSynthPlugin::getFramesNeeded(const OFX::FramesNeededArguments &p_Args, OFX::FramesNeededSetter &p_FramesNeededSetter)
{
    std::string script;
    m_scriptParam->getValueAtTime(p_Args.time, script);
    const int radius = !script.empty() ? m_radiusParam->getValueAtTime(p_Args.time) : 0;

    OfxRangeD frameRange;
    frameRange.min = p_Args.time - radius;
    frameRange.max = p_Args.time + radius;

    p_FramesNeededSetter.setFramesNeeded(*m_srcClip, frameRange);
}

void OFXVapourSynthPlugin::render(const OFX::RenderArguments& p_Args)
{
    if ((m_dstClip->getPixelDepth() == OFX::eBitDepthFloat || m_dstClip->getPixelDepth() == OFX::eBitDepthUShort) && (m_dstClip->getPixelComponents() == OFX::ePixelComponentRGBA))
    {
        setupAndProcess(p_Args);
    }
    else
    {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

struct OfxSourceData {
    OFX::Clip *srcClip;
    VSVideoInfo vi;
    int minFrame;
    int maxFrame;
};

static const VSFrame *VS_CC ofxSourceGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    OfxSourceData *d = (OfxSourceData *)instanceData;

    if (activationReason == arInitial) {
        std::unique_ptr<OFX::Image> srcImg(d->srcClip->fetchImage((double)n - d->minFrame));
        if (!srcImg) {
            vsapi->setFilterError("Failed to retrieve source frame", frameCtx);
            return nullptr;
        }

        VSFrame *f = vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, nullptr, core);
        auto rect = srcImg->getRegionOfDefinition();
        
        if (d->vi.format.sampleType == stFloat) {
            float *dstPtrR = (float *)vsapi->getWritePtr(f, 0);
            float *dstPtrG = (float *)vsapi->getWritePtr(f, 1);
            float *dstPtrB = (float *)vsapi->getWritePtr(f, 2);

            for (int y = rect.y2 - 1; y >= rect.y1; --y) {
                for (int x = rect.x1; x < rect.x2; ++x) {
                    const float *currPix = static_cast<const float *>(srcImg ? srcImg->getPixelAddress(x, y) : 0);
                    dstPtrR[x - rect.x1] = currPix[0];
                    dstPtrG[x - rect.x1] = currPix[1];
                    dstPtrB[x - rect.x1] = currPix[2];
                }
                dstPtrR += vsapi->getStride(f, 0) / sizeof(float);
                dstPtrG += vsapi->getStride(f, 1) / sizeof(float);
                dstPtrB += vsapi->getStride(f, 2) / sizeof(float);
            }
        } else {
            uint16_t *dstPtrR = (uint16_t *)vsapi->getWritePtr(f, 0);
            uint16_t *dstPtrG = (uint16_t *)vsapi->getWritePtr(f, 1);
            uint16_t *dstPtrB = (uint16_t *)vsapi->getWritePtr(f, 2);

            for (int y = rect.y2 - 1; y >= rect.y1; --y) {
                for (int x = rect.x1; x < rect.x2; ++x) {
                    const uint16_t *currPix = static_cast<const uint16_t *>(srcImg ? srcImg->getPixelAddress(x, y) : 0);
                    dstPtrR[x - rect.x1] = currPix[0];
                    dstPtrG[x - rect.x1] = currPix[1];
                    dstPtrB[x - rect.x1] = currPix[2];
                }
                dstPtrR += vsapi->getStride(f, 0) / sizeof(uint16_t);
                dstPtrG += vsapi->getStride(f, 1) / sizeof(uint16_t);
                dstPtrB += vsapi->getStride(f, 2) / sizeof(uint16_t);
            }
        }

        return f;
    }

    return nullptr;
}

static void VS_CC ofxSourceFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    OfxSourceData *d = (OfxSourceData *)instanceData;
    delete d;
}

bool OFXVapourSynthPlugin::OpenScript(const std::string &newScript, OFX::Clip *srcClip, int width, int height, int depth) {
    if (m_loadedScript != newScript || m_width != width || m_height != height || m_depth != depth) {
        // fixme, will we ever get here since the identity check exists? hopefully not
        if (newScript.empty())
            return false;
        m_loadedScript.clear();
        m_vsapi->freeNode(m_node);
        m_node = nullptr;
        m_vssapi->freeScript(m_script);
        m_script = nullptr;

        VSCore *core = m_vsapi->createCore(0);

        OfxSourceData *d = new OfxSourceData();
        d->vi.width = width;
        d->vi.height = height;

        auto range = srcClip->getFrameRange();
        d->srcClip = srcClip;
        d->minFrame = static_cast<int>(range.min);
        d->maxFrame = static_cast<int>(range.max);
        d->vi.numFrames = d->maxFrame - d->minFrame; // fixme, plus one here?
        m_width = width;
        m_height = height;
        m_depth = depth;
        m_vsapi->queryVideoFormat(&d->vi.format, cfRGB, (depth == 32) ? stFloat : stInteger, depth, 0, 0, core);

        VSNode *source = m_vsapi->createVideoFilter2("OFXSource", &d->vi, ofxSourceGetFrame, ofxSourceFree, fmUnordered, nullptr, 0, d, core);
        m_script = m_vssapi->createScript(core);
        VSMap *m = m_vsapi->createMap();
        m_vsapi->mapConsumeNode(m, "source", source, maAppend);
        m_vssapi->setVariables(m_script, m);
        m_vsapi->freeMap(m);

        if (m_vssapi->evaluateBuffer(m_script, newScript.c_str(), "OFX.vpy")) {
            m_lastErrorMsg = m_vssapi->getError(m_script);
            m_vssapi->freeScript(m_script);
            m_script = nullptr;
            return false;
        }

        m_node = m_vssapi->getOutputNode(m_script, 0);
        if (!m_node) {
            m_lastErrorMsg = "No output node set";
            m_vssapi->freeScript(m_script);
            m_script = nullptr;
            return false;
        }

        const VSVideoInfo *vi = m_vsapi->getVideoInfo(m_node);
        if (vi->width != width || vi->height != height || vi->format.bitsPerSample != depth || d->vi.numFrames != vi->numFrames) {
            m_lastErrorMsg = "Invalid output node set. Doesn't match input:";
            if (vi->width != width)
                m_lastErrorMsg += " width";
            if (vi->height != height)
                m_lastErrorMsg += " height";
            if (vi->format.bitsPerSample != depth)
                m_lastErrorMsg += " format";
            if (d->vi.numFrames != vi->numFrames)
                m_lastErrorMsg += " num_frames";
            m_vsapi->freeNode(m_node);
            m_node = nullptr;
            m_vssapi->freeScript(m_script);
            m_script = nullptr;
            return false;
        }

        m_lastErrorMsg.clear();
        m_loadedScript = newScript;
    }

    return true;
}

static std::wstring utf16_from_utf8(const std::string &str) {
    int required_size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wbuffer;
    wbuffer.resize(required_size - 1);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wbuffer[0], required_size);
    return wbuffer;
}


void OFXVapourSynthPlugin::setupAndProcess(const OFX::RenderArguments& p_Args)
{
    // Get the dst image
    std::unique_ptr<OFX::Image> dst(m_dstClip->fetchImage(p_Args.time));

    const OFX::BitDepthEnum dstBitDepth         = dst->getPixelDepth();
    const OFX::PixelComponentEnum dstComponents = dst->getPixelComponents();

    std::string scriptValue;
    m_scriptParam->getValueAtTime(p_Args.time, scriptValue);

    // Get the center src image to verify properties
    // fixme, can probably be replaced with a dst only check? need to verify later
    std::unique_ptr<OFX::Image> src(m_srcClip->fetchImage(p_Args.time));

    const OFX::BitDepthEnum srcBitDepth         = src->getPixelDepth();
    const OFX::PixelComponentEnum srcComponents = src->getPixelComponents();

    auto srcRect = src->getRegionOfDefinition();

    m_srcClip->getFrameRange();

    std::string scriptName;
    m_scriptParam->getValueAtTime(p_Args.time, scriptName);
    bool scriptValid = OpenScript(scriptValue, m_srcClip, srcRect.x2 - srcRect.x1, srcRect.y2 - srcRect.y1, (srcBitDepth == OFX::eBitDepthFloat) ? 32 : 16);
    if (!scriptValid)
    {
        OFX::throwSuiteStatusException(kOfxStatErrValue);
    }

    // Check to see if the bit depth and number of components are the same
    if ((srcBitDepth != dstBitDepth) || (srcComponents != dstComponents))
    {
        OFX::throwSuiteStatusException(kOfxStatErrValue);
    }

    char errMsg[256];
    assert(m_node);
    const VSFrame *f = m_vsapi->getFrame(static_cast<int>(p_Args.time - m_srcClip->getFrameRange().min), m_node, errMsg, sizeof(errMsg));
    if (!f) {
        if (errMsg != m_lastErrorMsg) {
            m_lastErrorMsg = errMsg;
        }
        OFX::throwSuiteStatusException(kOfxStatErrValue);
    }
    m_lastErrorMsg.clear();
    const VSVideoFormat *fi = m_vsapi->getVideoFrameFormat(f);
    auto procWindow = p_Args.renderWindow;

    if (fi->sampleType == stFloat) {
        const float *srcR = (const float *)m_vsapi->getReadPtr(f, 0);
        const float *srcG = (const float *)m_vsapi->getReadPtr(f, 1);
        const float *srcB = (const float *)m_vsapi->getReadPtr(f, 2);

        for (int y = procWindow.y2 - 1; y >= procWindow.y1; --y) {
            if (abort())
                break;

            float *dstPix = static_cast<float *>(dst->getPixelAddress(procWindow.x1, y));

            for (int x = procWindow.x1; x < procWindow.x2; ++x) {
                dstPix[0] = srcR[x - procWindow.x1];
                dstPix[1] = srcG[x - procWindow.x1];
                dstPix[2] = srcB[x - procWindow.x1];
                dstPix[3] = 1;

                dstPix += 4;
            }
            srcR += m_vsapi->getStride(f, 0) / sizeof(float);
            srcG += m_vsapi->getStride(f, 1) / sizeof(float);
            srcB += m_vsapi->getStride(f, 2) / sizeof(float);
        }
    } else {
        const uint16_t *srcR = (const uint16_t *)m_vsapi->getReadPtr(f, 0);
        const uint16_t *srcG = (const uint16_t *)m_vsapi->getReadPtr(f, 1);
        const uint16_t *srcB = (const uint16_t *)m_vsapi->getReadPtr(f, 2);

        for (int y = procWindow.y2 - 1; y >= procWindow.y1; --y) {
            if (abort())
                break;

            uint16_t *dstPix = static_cast<uint16_t *>(dst->getPixelAddress(procWindow.x1, y));

            for (int x = procWindow.x1; x < procWindow.x2; ++x) {
                dstPix[0] = srcR[x - procWindow.x1];
                dstPix[1] = srcG[x - procWindow.x1];
                dstPix[2] = srcB[x - procWindow.x1];
                dstPix[3] = std::numeric_limits<uint16_t>::max();

                dstPix += 4;
            }
            srcR += m_vsapi->getStride(f, 0) / sizeof(uint16_t);
            srcG += m_vsapi->getStride(f, 1) / sizeof(uint16_t);
            srcB += m_vsapi->getStride(f, 2) / sizeof(uint16_t);
        }
    }

    m_vsapi->freeFrame(f);
}

bool OFXVapourSynthPlugin::isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip *&identityClip, double &identityTime) {
    std::string scriptText;
    identityClip = m_srcClip;
    m_scriptParam->getValueAtTime(args.time, scriptText);
    return scriptText.empty();
}

void OFXVapourSynthPlugin::changedParam(const OFX::InstanceChangedArgs &/*args*/, const std::string &paramName) {
    if (paramName == "errorbutton") {
        if (!m_lastErrorMsg.empty())
            MessageBoxW(nullptr, utf16_from_utf8(m_lastErrorMsg).c_str(), L"VapourSynth Script Error", MB_OK);
    }
}

////////////////////////////////////////////////////////////////////////////////
OFXVapourSynthPluginFactory::OFXVapourSynthPluginFactory()
    : OFX::PluginFactoryHelper<OFXVapourSynthPluginFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}

void OFXVapourSynthPluginFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    // Basic labels
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    // Add the supported contexts, only filter at the moment
    p_Desc.addSupportedContext(OFX::eContextFilter);
    p_Desc.addSupportedContext(OFX::eContextGeneral); // why do all samples add general? is this some weird relic?

    // Add supported pixel depths
    p_Desc.addSupportedBitDepth(OFX::eBitDepthUShort);
    p_Desc.addSupportedBitDepth(OFX::eBitDepthFloat);

    // Set a few flags
    p_Desc.setSingleInstance(true);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setSupportsTiles(false);
    p_Desc.setTemporalClipAccess(true);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(false);

    // FIXME, should probably be set to true for later scaling plugins
    p_Desc.setSupportsMultiResolution(false);
    p_Desc.setSupportsMultipleClipDepths(false);
}

static OFX::StringParamDescriptor * DefineScriptParam(OFX::ImageEffectDescriptor& p_Desc, const std::string& p_Name, const std::string& p_Label,
                                                    const std::string& p_Hint, OFX::GroupParamDescriptor* p_Parent = NULL)
{
    OFX::StringParamDescriptor* param = p_Desc.defineStringParam(p_Name);
    param->setLabels(p_Label, p_Label, p_Label);
    param->setScriptName(p_Name);
    param->setStringType(OFX::eStringTypeMultiLine);
    param->setHint(p_Hint);
    param->setDefault("import vapoursynth as vs\n\nsource.set_output()");
    param->setCanUndo(true);

    if (p_Parent)
    {
        param->setParent(*p_Parent);
    }

    return param;
}

static OFX::IntParamDescriptor* DefineRadiusParam(OFX::ImageEffectDescriptor& p_Desc, const std::string& p_Name, const std::string& p_Label,
                                                   const std::string& p_Hint, OFX::GroupParamDescriptor* p_Parent = NULL)
{
    OFX::IntParamDescriptor* param = p_Desc.defineIntParam(p_Name);
    param->setLabels(p_Label, p_Label, p_Label);
    param->setScriptName(p_Name);
    param->setHint(p_Hint);
    param->setDefault(10);
    param->setRange(0, 30);
    param->setDisplayRange(0, 30);

    if (p_Parent)
    {
        param->setParent(*p_Parent);
    }

    return param;
}

static OFX::PushButtonParamDescriptor *DefineErrorButtonParam(OFX::ImageEffectDescriptor &p_Desc, const std::string &p_Name, const std::string &p_Label,
    const std::string &p_Hint, OFX::GroupParamDescriptor *p_Parent = NULL) {
    OFX::PushButtonParamDescriptor *param = p_Desc.definePushButtonParam(p_Name);
    param->setLabels(p_Label, p_Label, p_Label);
    param->setScriptName(p_Name);
    param->setHint(p_Hint);

    if (p_Parent) {
        param->setParent(*p_Parent);
    }

    return param;
}

void OFXVapourSynthPluginFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum /*p_Context*/)
{
    // Create the mandated source clip in filter context
    OFX::ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(true);
    srcClip->setSupportsTiles(false);
    srcClip->setIsMask(false);

    // Create the mandated output clip
    OFX::ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    //dstClip->addSupportedComponent(OFX::ePixelComponentAlpha); fixme, probably unsupported in the sample
    dstClip->setSupportsTiles(false);

    // Create page to add UI controllers
    OFX::PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    // Define slider params for blend and interval

    page->addChild(*DefineRadiusParam(p_Desc, "radius", "Radius", "Temporal radius needed by the script"));
    page->addChild(*DefineScriptParam(p_Desc, "script", "Script", "Example:\nimport vapoursynth as vs\nsource = source.std.FlipVertical()\nsource.set_output()"));
    page->addChild(*DefineErrorButtonParam(p_Desc, "errorbutton", "Show Error", "Displays the full error message in a dialog"));
}

OFX::ImageEffect* OFXVapourSynthPluginFactory::createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum /*p_Context*/)
{
    return new OFXVapourSynthPlugin(p_Handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    static OFXVapourSynthPluginFactory OFXVapourSynthPlugin;
    p_FactoryArray.push_back(&OFXVapourSynthPlugin);
}

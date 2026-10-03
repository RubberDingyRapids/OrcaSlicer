#include "wxMediaCtrl3.h"
#include "AVVideoDecoder.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/log/trivial.hpp>
#include <wx/dcclient.h>
#ifdef __WIN32__
#include <versionhelpers.h>
#include <wx/msw/registry.h>
#include <shellapi.h>
#endif

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

BEGIN_EVENT_TABLE(wxMediaCtrl3, wxWindow)

// catch paint events
EVT_PAINT(wxMediaCtrl3::paintEvent)

END_EVENT_TABLE()

struct StaticBambuLib : BambuLib
{
    static StaticBambuLib &get(BambuLib *);
};

wxMediaCtrl3::wxMediaCtrl3(wxWindow *parent)
    : wxWindow(parent, wxID_ANY)
    , BambuLib(StaticBambuLib::get(this))
    , m_thread([this] { PlayThread(); })
{
    SetBackgroundColour("#000001ff");
}

wxMediaCtrl3::~wxMediaCtrl3()
{
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_url.reset(new wxURI);
        m_frame = wxImage(m_idle_image);
        m_cond.notify_all();
    }
    // Clearing m_url only wakes the play thread if it is waiting on the condition
    // variable. While a stream is being negotiated it sits inside BambuSource
    // (Bambu_StartStream / Bambu_ReadSample), which is native and blocking, so it never
    // reaches the m_url check and join() would hang the UI thread for good - the app
    // freezes the moment this control is destroyed mid-negotiation. Closing the tunnel
    // makes that call return so the thread can unwind.
    BOOST_LOG_TRIVIAL(info) << "~wxMediaCtrl3: signalled, closing tunnel";
    if (void* tunnel = m_active_tunnel.exchange(nullptr, std::memory_order_acq_rel)) {
        BOOST_LOG_TRIVIAL(info) << "~wxMediaCtrl3: Bambu_Close entering";
        Bambu_Close(tunnel);
        BOOST_LOG_TRIVIAL(info) << "~wxMediaCtrl3: Bambu_Close returned";
    }
    BOOST_LOG_TRIVIAL(info) << "~wxMediaCtrl3: joining play thread";
    m_thread.join();
    BOOST_LOG_TRIVIAL(info) << "~wxMediaCtrl3: joined";
}

void wxMediaCtrl3::Load(wxURI url)
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_video_size = wxDefaultSize;
    m_error = 0;
    m_url.reset(new wxURI(url));
    m_cond.notify_all();
}

void wxMediaCtrl3::Play()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_state != wxMEDIASTATE_PLAYING) {
        m_state = wxMEDIASTATE_PLAYING;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void wxMediaCtrl3::Stop()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    m_url.reset();
    m_frame = wxImage(m_idle_image);
    NotifyStopped();
    m_cond.notify_all();
    Refresh();
}

void wxMediaCtrl3::SetIdleImage(wxString const &image)
{
    if (m_idle_image == image)
        return;
    m_idle_image = image;
    if (m_url == nullptr) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_frame = wxImage(m_idle_image);
        assert(m_frame.IsOk());
        Refresh();
    }
}

wxMediaState wxMediaCtrl3::GetState()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_state;
}

int wxMediaCtrl3::GetLastError()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_error;
}

wxSize wxMediaCtrl3::GetVideoSize()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    return m_video_size;
}

wxSize wxMediaCtrl3::DoGetBestSize() const
{
    return {-1, -1};
}

static void adjust_frame_size(wxSize & frame, wxSize const & video, wxSize const & window)
{
    if (video.x * window.y < video.y * window.x)
        frame = { video.x * window.y / video.y, window.y };
    else
        frame = { window.x, video.y * window.x / video.x };
}

void wxMediaCtrl3::paintEvent(wxPaintEvent &evt)
{
    wxPaintDC dc(this);
    auto      size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    std::unique_lock<std::mutex> lk(m_mutex);
    if (!m_frame.IsOk())
        return;
    auto size2 = m_frame.GetSize();
    if (size2.x != m_frame_size.x && size2.y == m_frame_size.y)
        size2.x = m_frame_size.x;
    auto size3 = (size - size2) / 2;
    if (size2.x != size.x && size2.y != size.y) {
        double scale = 1.;
        if (size.x * size2.y > size.y * size2.x) {
            size3 = {size.x * size2.y / size.y, size2.y};
            scale = double(size.y) / size2.y;
        } else {
            size3 = {size2.x, size.y * size2.x / size.x};
            scale = double(size.x) / size2.x;
        }
        dc.SetUserScale(scale, scale);
        size3 = (size3 - size2) / 2;
    }
    dc.DrawBitmap(m_frame, size3.x, size3.y);
}

void wxMediaCtrl3::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags == wxSIZE_USE_EXISTING) return;
    wxMediaCtrl_OnSize(this, m_video_size, width, height);
    std::unique_lock<std::mutex> lk(m_mutex);
    adjust_frame_size(m_frame_size, m_video_size, GetSize());
    Refresh();
}

void wxMediaCtrl3::bambu_log(void *ctx, int level, tchar const *msg2)
{
#ifdef _WIN32
    wxString msg(msg2);
#else
    wxString msg = wxString::FromUTF8(msg2);
#endif
    if (level == 1) {
        if (msg.EndsWith("]")) {
            int n = msg.find_last_of('[');
            if (n != wxString::npos) {
                long val = 0;
                wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
                if (msg.SubString(n + 1, msg.Length() - 2).ToLong(&val)) {
                    std::unique_lock<std::mutex> lk(ctrl->m_mutex);
                    ctrl->m_error = (int) val;
                }
            }
        } else if (msg.Contains("stat_log")) {
            wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
            wxMediaCtrl3 *ctrl = (wxMediaCtrl3 *) ctx;
            evt.SetEventObject(ctrl);
            evt.SetString(msg.Mid(msg.Find(' ') + 1));
            wxPostEvent(ctrl, evt);
        }
    }
    BOOST_LOG_TRIVIAL(info) << msg.ToUTF8().data();
}

void wxMediaCtrl3::PlayThread()
{
    using namespace std::chrono_literals;
    std::shared_ptr<wxURI> url;
    std::unique_lock<std::mutex> lk(m_mutex);
    while (true) {
        m_cond.wait(lk, [this, &url] { return m_url != url; });
        url = m_url;
        if (url == nullptr)
            continue;
        if (!url->HasScheme())
            break;
        lk.unlock();
        Bambu_Tunnel tunnel = nullptr;
        int error = Bambu_Create(&tunnel, m_url->BuildURI().ToUTF8());
        BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Create -> " << error;
        if (error == 0) {
            m_active_tunnel.store(tunnel, std::memory_order_release);
            Bambu_SetLogger(tunnel, &wxMediaCtrl3::bambu_log, this);
            error = Bambu_Open(tunnel);
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_Open -> " << error;
            if (error == 0)
                error = Bambu_would_block;
        }
        lk.lock();
        // Log what StartStream actually returns. It has never been recorded, so whether it
        // blocks indefinitely or keeps answering would_block is unknown - and the two have
        // completely different fixes. Log the first few and then sparsely, to stay readable.
        int start_tries = 0;
        while (error == int(Bambu_would_block)) {
            m_cond.wait_for(lk, 100ms);
            if (m_url != url) {
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: url changed after " << start_tries
                                        << " StartStream tries, abandoning";
                error = 1;
                break;
            }
            lk.unlock();
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: StartStream try " << (start_tries + 1) << " entering";
            error = Bambu_StartStream(tunnel, true);
            ++start_tries;
            if (start_tries <= 5 || start_tries % 20 == 0)
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_StartStream try " << start_tries
                                        << " -> " << error;
            lk.lock();
        }
        BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: StartStream loop done after " << start_tries
                                << " tries, error=" << error;
        Bambu_StreamInfo info;
        if (error == 0) {
            // StartStream needed ten attempts before it reported success, so the stream's
            // metadata is very likely not ready the instant it does. A single GetStreamInfo
            // here returned -1 and sent the thread down the failure path. Poll briefly, the
            // same way the start above does, and log the stream count so a wrong index is
            // distinguishable from metadata simply not being ready yet.
            const int stream_count = Bambu_GetStreamCount(tunnel);
            BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_GetStreamCount -> " << stream_count;
            int info_tries = 0;
            while (true) {
                error = Bambu_GetStreamInfo(tunnel, 0, &info);
                ++info_tries;
                BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: Bambu_GetStreamInfo try " << info_tries
                                        << " -> " << error;
                if (error == 0 || info_tries >= 30)
                    break;
                m_cond.wait_for(lk, 100ms);
                if (m_url != url) {
                    BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl3: url changed while waiting for stream info";
                    error = 1;
                    break;
                }
            }
        }
        AVVideoDecoder decoder;
        int minFrameDuration = 0;
        if (error == 0) {
            decoder.open(info);
            m_video_size = { info.format.video.width, info.format.video.height };
            adjust_frame_size(m_frame_size, m_video_size, GetSize());
            minFrameDuration = 800 / info.format.video.frame_rate; // 80%
            NotifyStopped();
        }
        Bambu_Sample sample;
        while (error == 0) {
            lk.unlock();
            error = Bambu_ReadSample(tunnel, &sample);
            lk.lock();
            while (error == int(Bambu_would_block)) {
                m_cond.wait_for(lk, 100ms);
                if (m_url != url) {
                    error = 1;
                    break;
                }
                lk.unlock();
                error = Bambu_ReadSample(tunnel, &sample);
                lk.lock();
            }
            if (error == 0) {
                auto frame_size = m_frame_size;
                lk.unlock();
                decoder.decode(sample);
#ifdef _WIN32
                wxBitmap bm;
                decoder.toWxBitmap(bm, frame_size);
#else
                wxImage bm;
                decoder.toWxImage(bm, frame_size);
#endif
                lk.lock();
                if (m_url != url) {
                    error = 1;
                    break;
                }
                if (bm.IsOk()) {
                    auto now = std::chrono::system_clock::now();
                    if (m_last_PTS && (sample.decode_time - m_last_PTS) < 30000000ULL) { // 3s
                        auto next_PTS_expected = m_last_PTS_expected + std::chrono::milliseconds((sample.decode_time - m_last_PTS) / 10000ULL);
                        // The frame is late, catch up a little
                        auto next_PTS_practical = m_last_PTS_practical + std::chrono::milliseconds(minFrameDuration);
                        auto next_PTS = std::max(next_PTS_expected, next_PTS_practical);
                        if(now < next_PTS)
                            std::this_thread::sleep_until(next_PTS);
                        else
                            next_PTS = now;
                        //auto text = wxString::Format(L"wxMediaCtrl3 pts diff %ld\n", std::chrono::duration_cast<std::chrono::milliseconds>(next_PTS - next_PTS_expected).count());
                        //OutputDebugString(text);
                        m_last_PTS = sample.decode_time;
                        m_last_PTS_expected = next_PTS_expected;
                        m_last_PTS_practical = next_PTS;
                    } else {
                        // Resync
                        m_last_PTS           = sample.decode_time;
                        m_last_PTS_expected  = now;
                        m_last_PTS_practical = now;
                    }
                    m_frame = bm;
                }
                CallAfter([this] { Refresh(); });
            }
        }
        if (tunnel) {
            lk.unlock();
            // The destructor may already have closed this to unblock us; whoever wins the
            // exchange performs the close, so it happens exactly once.
            if (void* t = m_active_tunnel.exchange(nullptr, std::memory_order_acq_rel))
                Bambu_Close(t);
            Bambu_Destroy(tunnel);
            tunnel = nullptr;
            lk.lock();
        }
        if (m_url == url)
            m_error = error;
        m_frame_size = wxDefaultSize;
        m_video_size = wxDefaultSize;
        NotifyStopped();
    }

}

void wxMediaCtrl3::NotifyStopped()
{
    m_state = wxMEDIASTATE_STOPPED;
    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
    event.SetId(GetId());
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

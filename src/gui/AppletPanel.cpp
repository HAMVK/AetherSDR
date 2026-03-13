#include "AppletPanel.h"
#include "RxApplet.h"

#include <QTabWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QLabel>

namespace AetherSDR {

static QWidget* placeholder(const QString& text)
{
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    auto* lbl = new QLabel(text);
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setStyleSheet("color: #405060; font-size: 11px;");
    l->addWidget(lbl);
    return w;
}

AppletPanel::AppletPanel(QWidget* parent) : QWidget(parent)
{
    setFixedWidth(260);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_tabs = new QTabWidget(this);
    m_tabs->setTabPosition(QTabWidget::North);
    m_tabs->setDocumentMode(true);

    // ── RX tab (implemented) ──────────────────────────────────────────────────
    m_rxApplet = new RxApplet;
    auto* rxScroll = new QScrollArea;
    rxScroll->setWidget(m_rxApplet);
    rxScroll->setWidgetResizable(true);
    rxScroll->setFrameShape(QFrame::NoFrame);
    m_tabs->addTab(rxScroll, "RX");

    // ── Placeholder tabs ──────────────────────────────────────────────────────
    m_tabs->addTab(placeholder("TX applet\n(coming soon)"),   "TX");
    m_tabs->addTab(placeholder("CW applet\n(coming soon)"),   "P/CW");
    m_tabs->addTab(placeholder("Phone applet\n(coming soon)"),"PHNE");
    m_tabs->addTab(placeholder("EQ applet\n(coming soon)"),   "EQ");
    m_tabs->addTab(placeholder("VU meter\n(coming soon)"),    "ANLG");

    root->addWidget(m_tabs);
}

void AppletPanel::setSlice(SliceModel* slice)
{
    m_rxApplet->setSlice(slice);
}

void AppletPanel::setAntennaList(const QStringList& ants)
{
    m_rxApplet->setAntennaList(ants);
}

} // namespace AetherSDR

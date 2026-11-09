/**
 * This file is part of the DOM implementation for KDE.
 *
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2010 Maksim Orlovich (maksim@kde.org)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "html/html_documentimpl.h"
#include "html/html_imageimpl.h"
#include "html/html_headimpl.h"
#include "html/html_baseimpl.h"
#include "html/htmltokenizer.h"
#include "html/html_formimpl.h"

#include "khtmlview.h"
#include "khtml_part.h"
#include "khtmlpart_p.h"
#include "khtml_settings.h"

#include "xml/xml_tokenizer.h"
#include "xml/dom2_eventsimpl.h"
#include "xml/dom_textimpl.h"

#include <khtml_global.h>
#include "rendering/render_object.h"
#include "dom/dom_exception.h"

#include "khtml_debug.h"
#include <QDBusConnection>
#include <QDBusReply>
#include <kcookiejar_interface.h>

#include "css/cssproperties.h"
#include "css/cssstyleselector.h"
#include "css/css_stylesheetimpl.h"
#include <stdlib.h>

using namespace DOM;
using namespace khtml;

// Same ceiling browsers use for a single cookie (name + attributes + value).
static const int kMaxCookieLength = 4096;

// How long we are willing to block the UI thread on the cookie daemon.
static const int kCookieDBusTimeoutMs = 3000;

HTMLDocumentImpl::HTMLDocumentImpl(KHTMLView *v)
    : DocumentImpl(v)
{
//    qCDebug(KHTML_LOG) << "HTMLDocumentImpl constructor this = " << this;
    htmlElement = nullptr;

    m_doAutoFill = false;
    m_determineParseMode = false;

    /* dynamic history stuff to be fixed later (pfeiffer)
        connect( KHTMLGlobal::vLinks(), SIGNAL(removed(QString)),
                 SLOT(slotHistoryChanged()));
    */
    connect(KHTMLGlobal::vLinks(), SIGNAL(inserted(QString)),
            SLOT(slotHistoryChanged()));
    connect(KHTMLGlobal::vLinks(), SIGNAL(cleared()),
            SLOT(slotHistoryChanged()));
}

HTMLDocumentImpl::~HTMLDocumentImpl()
{
}

DOMString HTMLDocumentImpl::referrer() const
{
    if (part()) {
        return part()->pageReferrer();
    }
    return DOMString();
}

DOMString HTMLDocumentImpl::lastModified() const
{
    if (part()) {
        return part()->lastModified();
    }
    return DOMString();
}

DOMString HTMLDocumentImpl::cookie() const
{
    WId windowId = 0;
    KHTMLView *v = view();

    if (v && v->topLevelWidget()) {
        windowId = v->topLevelWidget()->winId();
    }

    org::kde::KCookieServer kcookiejar("org.kde.kded5", "/modules/kcookiejar", QDBusConnection::sessionBus());
    // This is a synchronous call from script; don't let a wedged kded freeze the
    // page for the default 25 seconds.
    kcookiejar.setTimeout(kCookieDBusTimeoutMs);
    QDBusReply<QString> reply = kcookiejar.findDOMCookies(URL().url(), qlonglong(windowId));

    if (!reply.isValid()) {
        qCWarning(KHTML_LOG) << "Can't communicate with cookiejar!";
        return DOMString();
    }

    return DOMString(reply.value());
}

void HTMLDocumentImpl::setCookie(const DOMString &value)
{
    // The value is spliced into a fake "Set-Cookie: ...\n" header block that the
    // cookie jar parses line by line. A script-supplied CR/LF would therefore be
    // able to smuggle in extra Set-Cookie lines with attributes (Domain, HttpOnly,
    // ...) that document.cookie must never be able to set. Like other browsers,
    // refuse cookies containing control separators or that are absurdly large.
    const QString raw = value.string();
    if (raw.size() > kMaxCookieLength ||
            raw.contains(QLatin1Char('\n')) ||
            raw.contains(QLatin1Char('\r')) ||
            raw.contains(QChar(0))) {
        return;
    }

    WId windowId = 0;
    KHTMLView *v = view();

    if (v && v->topLevelWidget()) {
        windowId = v->topLevelWidget()->winId();
    }

    QByteArray fake_header("Set-Cookie: ");
    fake_header.append(raw.toLatin1().constData());
    fake_header.append("\n");
    // Note that kded modules are autoloaded so we don't need to call loadModule ourselves.
    org::kde::KCookieServer kcookiejar("org.kde.kded5", "/modules/kcookiejar", QDBusConnection::sessionBus());
    // Can't use kcookiejar.addCookies because then we can't pass NoBlock...
    kcookiejar.call(QDBus::NoBlock, "addCookies",
                    URL().url(), fake_header, qlonglong(windowId));
}

void HTMLDocumentImpl::setBody(HTMLElementImpl *_body, int &exceptioncode)
{
    // document.body may only be a <body> or a <frameset>
    if (!_body || (_body->id() != ID_BODY && _body->id() != ID_FRAMESET)) {
        exceptioncode = DOMException::HIERARCHY_REQUEST_ERR;
        return;
    }

    // Without a root element there is nothing to hang the body on (this used to
    // dereference a null documentElement()).
    ElementImpl *root = documentElement();
    if (!root) {
        exceptioncode = DOMException::HIERARCHY_REQUEST_ERR;
        return;
    }

    HTMLElementImpl *b = body();
    if (!b) {
        root->appendChild(_body, exceptioncode);
    } else {
        root->replaceChild(_body, b, exceptioncode);
    }
}

Tokenizer *HTMLDocumentImpl::createTokenizer()
{
    return new HTMLTokenizer(docPtr(), m_view);
}

// --------------------------------------------------------------------------
// not part of the DOM
// --------------------------------------------------------------------------

bool HTMLDocumentImpl::childAllowed(NodeImpl *newChild)
{
    // ### support comments. etc as a child
    return (newChild->id() == ID_HTML || newChild->id() == ID_COMMENT || newChild->nodeType() == Node::DOCUMENT_TYPE_NODE);
}

ElementImpl *HTMLDocumentImpl::createElement(const DOMString &name, int *pExceptioncode)
{
    if (pExceptioncode && !Element::khtmlValidQualifiedName(name)) {
        *pExceptioncode = DOMException::INVALID_CHARACTER_ERR;
        return nullptr;
    }

    return createHTMLElement(name, hMode != XHtml);
}

ElementImpl *HTMLDocumentImpl::activeElement() const
{
    NodeImpl *fn = focusNode();
    if (!fn || !fn->isElementNode()) {
        return body();
    } else {
        return static_cast<ElementImpl *>(fn);
    }
}

void HTMLDocumentImpl::slotHistoryChanged()
{
    // Restyling visited links on history changes is disabled for now (it used to
    // sit behind an unconditional "if (true || ...) return;"). The old body is
    // kept below for whoever turns it back on.
    return;

#if 0
    if (!m_render) {
        return;
    }

    recalcStyle(Force);
    m_render->repaint();
#endif
}

HTMLMapElementImpl *HTMLDocumentImpl::getMap(const DOMString &_url)
{
    const QString url = _url.string();

    // usemap="#name": the map is whatever follows the '#'. A reference without
    // a '#' is looked up as-is, as before.
    const int pos = url.indexOf(QLatin1Char('#'));
    const QString s = (pos >= 0) ? url.mid(pos + 1) : url;

    QMap<QString, HTMLMapElementImpl *>::const_iterator it = mapMap.constFind(s);

    if (it != mapMap.constEnd()) {
        return *it;
    } else {
        return nullptr;
    }
}

void HTMLDocumentImpl::contentLoaded()
{
    // auto fill: walk the tree and try to fill in login credentials
    if (view() && m_doAutoFill) {
        // Clear the flag first so a re-entrant call (autofill can run script
        // and spin the event loop) can't fill the same forms twice.
        m_doAutoFill = false;

        for (NodeImpl *n = this; n; n = n->traverseNextNode()) {
            if (n->id() == ID_FORM) {
                static_cast<HTMLFormElementImpl *>(n)->doAutoFill();
            }
        }
    }
}

void HTMLDocumentImpl::close()
{
    bool doload = !parsing() && m_tokenizer;

    DocumentImpl::close();

    if (doload) {

        if (title().isEmpty()) { // ensure setTitle is called at least once
            setTitle(DOMString());
        }

        // According to dom the load event must not bubble
        // but other browsers execute in a frameset document
        // the first(IE)/last(Moz/Konq) registered onload on a <frame> and the
        // first(IE)/last(Moz/Konq) registered onload on a <frameset>.

        //qCDebug(KHTML_LOG) << "dispatching LOAD_EVENT on document " << document() << " " << (view()?view()->part()->name():0);

        //Make sure to flush any pending image events now, as we want them out before the document's load event
        dispatchImageLoadEventsNow();
        document()->dispatchWindowEvent(EventImpl::LOAD_EVENT, false, false);

        // don't update rendering if we're going to redirect anyway
        // (part() is re-evaluated: the load handler may have navigated away)
        if (part() && (part()->d->m_redirectURL.isEmpty() ||
                       part()->d->m_delayRedirect > 1)) {
            updateRendering();
        }
    }
}

void HTMLDocumentImpl::determineParseMode()
{
    m_determineParseMode = true;
    if (m_doctype == nullptr) {
        // Currently we haven't got any doctype, so default to quirks mode and Html4
        changeModes(Compat, Html4);
    }
}

void HTMLDocumentImpl::changeModes(ParseMode newPMode, HTMLMode newHMode)
{
    if (!m_determineParseMode) { // change mode only when KHTMLPart called determineParseMode
        return;
    }
    ParseMode oldPMode = pMode;
    pMode = newPMode;
    hMode = newHMode;
    // This needs to be done last, see tests/parser/compatmode_xhtml_mixed.html
    if (hMode == Html4 && !m_htmlRequested) {
        // this part is still debatable and possibly UA dependent
        hMode = XHtml;
        pMode = Transitional;
    }
    m_htmlCompat = (hMode != XHtml);

    // The style selector doesn't exist yet for documents that haven't been
    // attached to a view; it picks the mode up when it is created.
    if (m_styleSelector) {
        m_styleSelector->strictParsing = !inCompatMode();
    }

#if 0
    qCDebug(KHTML_LOG) << "DocumentImpl::determineParseMode: publicId =" << publicID << " systemId = " << systemID;
    qCDebug(KHTML_LOG) << "DocumentImpl::determineParseMode: htmlMode = " << hMode;
    if (pMode == Strict) {
        qCDebug(KHTML_LOG) << " using strict parseMode";
    } else if (pMode == Compat) {
        qCDebug(KHTML_LOG) << " using compatibility parseMode";
    } else {
        qCDebug(KHTML_LOG) << " using transitional parseMode";
    }
#endif

    if (pMode != oldPMode && styleSelector()) {
        updateStyleSelector(true/*shallow*/);
    }
}

NodeListImpl *HTMLDocumentImpl::getElementsByName(const DOMString &elementName)
{
    return new NameNodeListImpl(this, elementName);
}

HTMLCollectionImpl *HTMLDocumentImpl::images()
{
    return new HTMLCollectionImpl(this, HTMLCollectionImpl::DOC_IMAGES);
}

HTMLCollectionImpl *HTMLDocumentImpl::applets()
{
    return new HTMLCollectionImpl(this, HTMLCollectionImpl::DOC_APPLETS);
}

HTMLCollectionImpl *HTMLDocumentImpl::links()
{
    return new HTMLCollectionImpl(this, HTMLCollectionImpl::DOC_LINKS);
}

HTMLCollectionImpl *HTMLDocumentImpl::forms()
{
    return new HTMLCollectionImpl(this, HTMLCollectionImpl::DOC_FORMS);
}

HTMLCollectionImpl *HTMLDocumentImpl::layers()
{
    return new HTMLCollectionImpl(this, HTMLCollectionImpl::DOC_LAYERS);
}

HTMLCollectionImpl *HTMLDocumentImpl::anchors()
{
    return new HTMLCollectionImpl(this, HTMLCollectionImpl::DOC_ANCHORS);
}

HTMLCollectionImpl *HTMLDocumentImpl::all()
{
    return new HTMLCollectionImpl(this, HTMLCollectionImpl::DOC_ALL);
}

HTMLCollectionImpl *HTMLDocumentImpl::scripts()
{
    return new HTMLCollectionImpl(this, HTMLCollectionImpl::DOC_SCRIPTS);
}

// --------------------------------------------------------------------------
// Support for displaying plaintext
// --------------------------------------------------------------------------

class HTMLTextTokenizer: public khtml::Tokenizer
{
public:
    HTMLTextTokenizer(DOM::HTMLDocumentImpl *doc): m_doc(doc)
    {}

    void begin() override;
    void write(const TokenizerString &str, bool appendData) override;

    void end() override
    {
        emit finishedParsing();
    }
    void finish() override
    {
        end();
    }

    // We don't support any inline scripts here
    bool isWaitingForScripts() const override
    {
        return false;
    }
    bool isExecutingScript() const override
    {
        return false;
    }
    void executeScriptsWaitingForStylesheets() override {}
private:
    DOM::HTMLDocumentImpl *m_doc;
};

void HTMLTextTokenizer::begin()
{
    int dummy = 0;

    // Hold references while we build the skeleton so nothing leaks if an
    // appendChild() is refused (the DOM only takes its own reference on success).
    RefPtr<DOM::ElementImpl> html = m_doc->createElement("html", &dummy);
    RefPtr<DOM::ElementImpl> head = m_doc->createElement("head", &dummy);
    RefPtr<DOM::ElementImpl> body = m_doc->createElement("body", &dummy);
    RefPtr<DOM::ElementImpl> pre  = m_doc->createElement("pre", &dummy);

    if (!html || !head || !body || !pre) {
        return;
    }

    m_doc->appendChild(html.get(), dummy);
    html->appendChild(head.get(), dummy);
    html->appendChild(body.get(), dummy);
    body->appendChild(pre.get(), dummy);
}

void HTMLTextTokenizer::write(const TokenizerString &str, bool /*appendData*/)
{
    // A potential worry here is the document being modified by
    // a script while we're still loading. To handle that, we always look up
    // the pre again, and append to it, even for document.write mess
    // and the like.
    RefPtr<NodeListImpl> coll = m_doc->getElementsByTagName("pre");

    // item(0) stops at the first match; the old code called length() first,
    // which walked the *entire* document on every chunk (quadratic for big files).
    NodeImpl *pre = coll->item(0);
    if (!pre) {
        return;
    }

    int exceptionCode = 0;
    const QString chunk = str.toString();

    // Grow the existing trailing text node rather than creating one node per
    // network chunk: a large plain-text file used to produce thousands of
    // sibling text nodes.
    NodeImpl *last = pre->lastChild();
    if (last && last->nodeType() == Node::TEXT_NODE) {
        static_cast<TextImpl *>(last)->appendData(DOMString(chunk), exceptionCode);
        if (!exceptionCode) {
            return;
        }
        exceptionCode = 0; // fall back to a fresh node
    }

    RefPtr<TextImpl> text = m_doc->createTextNode(chunk);
    if (text) {
        pre->appendChild(text.get(), exceptionCode);
    }
}

HTMLTextDocumentImpl::HTMLTextDocumentImpl(KHTMLView *v): HTMLDocumentImpl(v)
{}

khtml::Tokenizer *HTMLTextDocumentImpl::createTokenizer()
{
    return new HTMLTextTokenizer(this);
}

#include "moc_html_documentimpl.cpp"

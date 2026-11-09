/*
 * Copyright (C) 2004 Apple Computer, Inc.  All rights reserved.
 * Copyright (C) 2005 Zack Rusin <zack@kde.org>
 * Copyright (C) 2007, 2008 Maksim Orlovich <maksim@kde.org>
 * Copyright (C) 2007, 2008 Fredrik Höglund <fredrik@kde.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Portions of this code are (c) by Apple Computer, Inc. and were licensed
 * under the following terms:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Revised implementation. The public interface (html_canvasimpl.h) is unchanged;
 * all changes are internal: crash/UB fixes, spec conformance, and hardening.
 * See the summary delivered with this file for the full list.
 */

#include "html_canvasimpl.h"
#include "html_documentimpl.h"

#include <khtmlview.h>
#include <khtml_part.h>

#include <dom/dom_exception.h>
#include <rendering/render_canvasimage.h>
#include <rendering/render_flow.h>
#include <css/cssstyleselector.h>
#include <css/cssproperties.h>
#include <css/cssparser.h>
#include <css/cssvalues.h>
#include <css/csshelper.h>
#include <xml/dom2_eventsimpl.h>
#include <html/html_imageimpl.h>
#include <misc/helper.h> // for colorFromCSSValue
#include <misc/translator.h>
#include <misc/imagefilter.h>
#include <imload/canvasimage.h>
#include <imload/imagemanager.h>
#include <kjs/global.h>
#include <kjs/operations.h> //uglyyy: needs for inf/NaN tests

#include <QtAlgorithms>
#include <QCharRef>
#include <QPoint>
#include <QLocale>
#include <QImage>
#include <QBuffer>
#include <QImageWriter>
#include <QVector>
#include <QByteArray>
#include <QBrush>
#include <QPen>
#include <QLineF>
#include <QPolygonF>
#include <QTransform>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include "khtml_debug.h"
#include <cmath>
#include <limits>

using namespace DOM;
using namespace khtml;
using namespace std;

// on Windows fmod might be a macro so std::fmod will not work
#ifdef fmod
#undef fmod
#endif

// MSVC only defines these with _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

// -------------------------------------------------------------------------
// Local helpers
// -------------------------------------------------------------------------

static const int kDefaultCanvasWidth  = 300;
static const int kDefaultCanvasHeight = 150;

static inline bool isInfArg(float x)
{
    return KJS::isInf(x) || KJS::isNaN(x);
}

// True if any of the arguments is NaN or +/-Infinity. The canvas spec says
// that such calls are silently ignored.
static inline bool anyNonFinite()
{
    return false;
}

template <typename... Rest>
static inline bool anyNonFinite(float first, Rest... rest)
{
    return isInfArg(first) || anyNonFinite(rest...);
}

// Rounds without invoking undefined behaviour on huge values.
static inline int safeRound(float v)
{
    const float lim = 1.0e9f;
    return qRound(qBound(-lim, v, lim));
}

static inline float degrees(float radians)
{
    return radians * 180.0 / M_PI;
}

// A closed, transformed rectangle as a path (device space).
static QPainterPath transformedRectPath(const QRectF &r, const QTransform &t)
{
    QPainterPath p;
    p.setFillRule(Qt::WindingFill);
    p.addPolygon(t.map(QPolygonF(r)));
    p.closeSubpath();
    return p;
}

static int parseCanvasDimension(AttributeImpl *attr, int fallback)
{
    bool ok = false;
    const int val = attr->val() ? attr->val()->toInt(&ok) : -1;
    return (ok && val > 0) ? val : fallback;
}

// -------------------------------------------------------------------------

HTMLCanvasElementImpl::HTMLCanvasElementImpl(DocumentImpl *doc)
    : HTMLElementImpl(doc)
{
    w = kDefaultCanvasWidth;
    h = kDefaultCanvasHeight;
    unsafe = false;
}

HTMLCanvasElementImpl::~HTMLCanvasElementImpl()
{
    if (context) {
        context->canvasElement = nullptr;
    }
}

void HTMLCanvasElementImpl::parseAttribute(AttributeImpl *attr)
{
    switch (attr->id()) {
    // ### TODO: making them reflect w/h -- how?
    case ATTR_WIDTH: {
        int nw = parseCanvasDimension(attr, kDefaultCanvasWidth);
        // Refuse sizes that would make the backing store unreasonably big.
        if (!khtmlImLoad::ImageManager::isAcceptableSize(unsigned(nw), unsigned(h))) {
            nw = kDefaultCanvasWidth;
        }
        w = nw;

        if (context) {
            context->resetContext(w, h);
        }
        setChanged();
        break;
    }
    case ATTR_HEIGHT: {
        int nh = parseCanvasDimension(attr, kDefaultCanvasHeight);
        if (!khtmlImLoad::ImageManager::isAcceptableSize(unsigned(w), unsigned(nh))) {
            nh = kDefaultCanvasHeight;
        }
        h = nh;

        if (context) {
            context->resetContext(w, h);
        }
        setChanged();
        break;
    }
    default:
        HTMLElementImpl::parseAttribute(attr);
    }
}

NodeImpl::Id HTMLCanvasElementImpl::id() const
{
    return ID_CANVAS;
}

void HTMLCanvasElementImpl::attach()
{
    assert(!attached());
    assert(!m_render);
    assert(parentNode());

    RenderStyle *_style = document()->styleSelector()->styleForElement(this);
    _style->ref();
    if (parentNode()->renderer() && parentNode()->renderer()->childAllowed() &&
            _style->display() != NONE) {
        m_render = new(document()->renderArena()) RenderCanvasImage(this);
        m_render->setStyle(_style);
        parentNode()->renderer()->addChild(m_render, nextRenderer());
    }
    _style->deref();

    NodeBaseImpl::attach();
    if (m_render) {
        m_render->updateFromElement();
    }
}

CanvasContext2DImpl *HTMLCanvasElementImpl::getContext2D()
{
    if (!context) {
        context = new CanvasContext2DImpl(this, w, h);
    }
    return context.get();
}

khtmlImLoad::CanvasImage *HTMLCanvasElementImpl::getCanvasImage()
{
    return getContext2D()->canvasImage;
}

bool HTMLCanvasElementImpl::isUnsafe() const
{
    return unsafe;
}

void HTMLCanvasElementImpl::markUnsafe()
{
    unsafe = true;
}

QString HTMLCanvasElementImpl::toDataURL(int &exceptionCode)
{
    return toDataURL(QString(), -1.0, exceptionCode);
}

QString HTMLCanvasElementImpl::toDataURL(const QString &typeIn, double quality, int &exceptionCode)
{
    exceptionCode = 0;
    if (isUnsafe()) {
        exceptionCode = DOMException::INVALID_ACCESS_ERR;
        return QString();
    }

    khtmlImLoad::CanvasImage *ci = getCanvasImage();
    context->syncBackBuffer();

    // Pick the encoder; anything we can't produce falls back to PNG, per spec.
    QString type = typeIn.trimmed().toLower();
    const char *format = "PNG";
    QString mime = QString::fromLatin1("image/png");
    if (type == QLatin1String("image/jpeg")) {
        format = "JPEG";
        mime = type;
    } else if (type == QLatin1String("image/webp") &&
               QImageWriter::supportedImageFormats().contains("webp")) {
        format = "WEBP";
        mime = type;
    }

    QImage out;
    if (qstrcmp(format, "JPEG") == 0) {
        // JPEG has no alpha: composite over opaque black, as the spec says.
        out = QImage(ci->qimage()->size(), QImage::Format_RGB32);
        out.fill(Qt::black);
        QPainter p(&out);
        p.drawImage(0, 0, *ci->qimage());
        p.end();
    } else {
        out = *ci->qimage();
    }

    const int q = (quality >= 0.0 && quality <= 1.0) ? qRound(quality * 100.0) : -1;

    QByteArray bytes;
    QBuffer sink(&bytes);
    if (!sink.open(QIODevice::WriteOnly)) {
        return QString::fromLatin1("data:,");
    }
    const bool saved = out.save(&sink, format, q);
    sink.close();

    if (!saved || bytes.isEmpty()) {
        return QString::fromLatin1("data:,"); // what the spec says for "can't encode"
    }

    return QString::fromLatin1("data:") + mime + QString::fromLatin1(";base64,") +
           QString::fromLatin1(bytes.toBase64());
}

// -------------------------------------------------------------------------
CanvasContext2DImpl::CanvasContext2DImpl(HTMLCanvasElementImpl *element, int width, int height):
    canvasElement(element), canvasImage(nullptr)
{
    resetContext(width, height);
}

CanvasContext2DImpl::~CanvasContext2DImpl()
{
    if (workPainter.isActive()) {
        workPainter.end();    // Make sure to stop it before blowing the image away!
    }
    delete canvasImage;
}

// Basic infrastructure..
void CanvasContext2DImpl::resetContext(int width, int height)
{
    // ### FIXME FIXME: use khtmlImLoad's limit policy
    // for physical canvas and transform painter to match logical resolution
    if (workPainter.isActive()) {
        workPainter.end();
    }

    if (canvasImage) {
        canvasImage->resizeImage(width, height);
    } else {
        canvasImage = new khtmlImLoad::CanvasImage(width, height);
    }
    canvasImage->qimage()->fill(0x00000000); // transparent black is the initial state

    stateStack.clear();

    PaintState defaultState;
    beginPath();
    defaultState.infinityTransform = false;
    defaultState.clipPath = QPainterPath();
    defaultState.clipPath.setFillRule(Qt::WindingFill);
    defaultState.clipping = false;

    defaultState.globalAlpha = 1.0f;
    defaultState.imageSmoothing = true;
    defaultState.globalCompositeOperation = QPainter::CompositionMode_SourceOver;

    defaultState.strokeStyle = new CanvasColorImpl(QColor(Qt::black));
    defaultState.fillStyle   = new CanvasColorImpl(QColor(Qt::black));

    defaultState.lineWidth  = 1.0f;
    defaultState.lineCap    = Qt::FlatCap;
    defaultState.lineJoin   = Qt::SvgMiterJoin;
    defaultState.miterLimit = 10.0f;
    defaultState.lineDash.clear();
    defaultState.lineDashOffset = 0.0f;

    defaultState.shadowOffsetX = 0.0f;
    defaultState.shadowOffsetY = 0.0f;
    defaultState.shadowBlur    = 0.0f;
    defaultState.shadowColor   = QColor(0, 0, 0, 0); // Transparent black

    stateStack.push(defaultState);

    dirty = DrtAll;
    needRendererUpdate();
    emptyPath = true;
}

void CanvasContext2DImpl::save()
{
    stateStack.push(stateStack.top());
}

void CanvasContext2DImpl::restore()
{
    if (stateStack.size() <= 1) {
        return;
    }

    stateStack.pop();
    dirty = DrtAll;
}

QPainter *CanvasContext2DImpl::acquirePainter()
{
    if (!workPainter.isActive()) {
        workPainter.begin(canvasImage->qimage());
        workPainter.setRenderHint(QPainter::Antialiasing);
        dirty = DrtAll;
    }

    PaintState &state = activeState();

    if (dirty & DrtClip) {
        if (state.clipping) {
            workPainter.setClipPath(state.clipPath);
        } else {
            workPainter.setClipping(false);
        }
    }

    if (dirty & DrtSmooth) {
        workPainter.setRenderHint(QPainter::SmoothPixmapTransform, state.imageSmoothing);
    }
    if (dirty & DrtAlpha) {
        workPainter.setOpacity(state.globalAlpha);
    }
    if (dirty & DrtCompOp) {
        workPainter.setCompositionMode(state.globalCompositeOperation);
    }
    if (dirty & DrtStroke) {
        QPen pen;
        pen.setWidthF(state.lineWidth); // was setWidth(int): truncated fractional widths
        pen.setCapStyle(state.lineCap);
        pen.setJoinStyle(state.lineJoin);
        pen.setMiterLimit(state.miterLimit);

        CanvasStyleBaseImpl *style = state.strokeStyle.get();
        if (style->type() == CanvasStyleBaseImpl::Color) {
            pen.setColor(static_cast<CanvasColorImpl *>(style)->color);
        } else {
            pen.setBrush(style->toBrush());
        }
        workPainter.setPen(pen); // only used as a carrier for the stroke brush; see drawPath()
    }
    if (dirty & DrtFill) {
        workPainter.setBrush(state.fillStyle->toBrush());
    }

    dirty = 0;

    needRendererUpdate();
    return &workPainter;
}

QImage CanvasContext2DImpl::extractImage(ElementImpl *el, int &exceptionCode, bool &unsafeOut) const
{
    QImage pic;

    exceptionCode = 0;
    unsafeOut = false;

    if (!el) {
        exceptionCode = DOMException::TYPE_MISMATCH_ERR;
        return pic;
    }

    if (el->id() == ID_CANVAS) {
        HTMLCanvasElementImpl *otherEl = static_cast<HTMLCanvasElementImpl *>(el);
        CanvasContext2DImpl *other = otherEl->getContext2D();
        other->syncBackBuffer();
        pic = *other->canvasImage->qimage(); // implicit-shared copy; safe even if el == our canvas

        if (otherEl->isUnsafe()) {
            unsafeOut = true;
        }
    } else if (el->id() == ID_IMG) {
        HTMLImageElementImpl *img = static_cast<HTMLImageElementImpl *>(el);
        if (img->complete()) {
            pic = img->currentImage();
        } else {
            exceptionCode = DOMException::INVALID_STATE_ERR;
        }

        if (img->isUnsafe()) {
            unsafeOut = true;
        }
    } else {
        exceptionCode = DOMException::TYPE_MISMATCH_ERR;
    }

    return pic;
}

void CanvasContext2DImpl::needRendererUpdate()
{
    needsCommit = true;
    if (canvasElement) {
        canvasElement->setChanged();
    }
}

void CanvasContext2DImpl::syncBackBuffer()
{
    if (workPainter.isActive()) {
        workPainter.end();
    }
}

void CanvasContext2DImpl::commit()
{
    syncBackBuffer();

    // Flush caches if we have changes.
    if (needsCommit) {
        canvasImage->contentUpdated();
        needsCommit = false;
    }
}

HTMLCanvasElementImpl *CanvasContext2DImpl::canvas() const
{
    return canvasElement;
}

// Transformation ops
//
// Per the current spec, calls with non-finite arguments are ignored (they used
// to poison the transform until the next setTransform()).

void CanvasContext2DImpl::scale(float x, float y)
{
    if (anyNonFinite(x, y)) {
        return;
    }

    dirty |= DrtTransform;
    activeState().transform.scale(x, y);
}

void CanvasContext2DImpl::rotate(float angle)
{
    if (anyNonFinite(angle)) {
        return;
    }

    dirty |= DrtTransform;
    activeState().transform.rotateRadians(angle);
}

void CanvasContext2DImpl::translate(float x, float y)
{
    if (anyNonFinite(x, y)) {
        return;
    }

    dirty |= DrtTransform;
    activeState().transform.translate(x, y);
}

void CanvasContext2DImpl::transform(float m11, float m12, float m21, float m22, float dx, float dy)
{
    if (anyNonFinite(m11, m12, m21, m22, dx, dy)) {
        return;
    }

    dirty |= DrtTransform;
    activeState().transform *= QTransform(m11, m12, 0.0f, m21, m22, 0.0f, dx, dy, 1.0f);
}

void CanvasContext2DImpl::setTransform(float m11, float m12, float m21, float m22, float dx, float dy)
{
    if (anyNonFinite(m11, m12, m21, m22, dx, dy)) {
        return;
    }

    activeState().transform.reset();
    activeState().infinityTransform = false;
    transform(m11, m12, m21, m22, dx, dy);
}

void CanvasContext2DImpl::resetTransform()
{
    activeState().transform.reset();
    activeState().infinityTransform = false;
    dirty |= DrtTransform;
}

// Composition state setting
//

float CanvasContext2DImpl::globalAlpha() const
{
    return activeState().globalAlpha;
}

void CanvasContext2DImpl::setGlobalAlpha(float a)
{
    if (isInfArg(a) || a < 0.0f || a > 1.0f) {
        return;
    }

    activeState().globalAlpha = a;
    dirty |= DrtAlpha;
}

static const IDTranslator<QString, QPainter::CompositionMode, const char *>::Info compModeTranslatorTable[] = {
    {"source-over", QPainter::CompositionMode_SourceOver},
    {"source-out",  QPainter::CompositionMode_SourceOut},
    {"source-in",   QPainter::CompositionMode_SourceIn},
    {"source-atop", QPainter::CompositionMode_SourceAtop},
    {"destination-atop", QPainter::CompositionMode_DestinationAtop},
    {"destination-in",   QPainter::CompositionMode_DestinationIn},
    {"destination-out",  QPainter::CompositionMode_DestinationOut},
    {"destination-over", QPainter::CompositionMode_DestinationOver},
    {"lighter", QPainter::CompositionMode_Plus},
    {"copy",    QPainter::CompositionMode_Source},
    {"xor",     QPainter::CompositionMode_Xor},
    // Blend modes
    {"multiply",    QPainter::CompositionMode_Multiply},
    {"screen",      QPainter::CompositionMode_Screen},
    {"overlay",     QPainter::CompositionMode_Overlay},
    {"darken",      QPainter::CompositionMode_Darken},
    {"lighten",     QPainter::CompositionMode_Lighten},
    {"color-dodge", QPainter::CompositionMode_ColorDodge},
    {"color-burn",  QPainter::CompositionMode_ColorBurn},
    {"hard-light",  QPainter::CompositionMode_HardLight},
    {"soft-light",  QPainter::CompositionMode_SoftLight},
    {"difference",  QPainter::CompositionMode_Difference},
    {"exclusion",   QPainter::CompositionMode_Exclusion},
    {nullptr, (QPainter::CompositionMode)0}
};

MAKE_TRANSLATOR(compModeTranslator, QString, QPainter::CompositionMode, const char *, compModeTranslatorTable)

DOM::DOMString CanvasContext2DImpl::globalCompositeOperation() const
{
    return compModeTranslator()->toLeft(activeState().globalCompositeOperation);
}

void CanvasContext2DImpl::setGlobalCompositeOperation(const DOM::DOMString &op)
{
    QString opStr = op.string();
    if (!compModeTranslator()->hasLeft(opStr)) {
        return;    // Ignore unknown
    }
    activeState().globalCompositeOperation = compModeTranslator()->toRight(opStr);
    dirty |= DrtCompOp;
}

bool CanvasContext2DImpl::imageSmoothingEnabled() const
{
    return activeState().imageSmoothing;
}

void CanvasContext2DImpl::setImageSmoothingEnabled(bool enabled)
{
    activeState().imageSmoothing = enabled;
    dirty |= DrtSmooth;
}

// Colors and styles.
//

static QColor colorFromString(DOM::DOMString domStr)
{
    // We make a temporary CSS decl. object to parse the color using the CSS parser.
    CSSStyleDeclarationImpl  tempStyle(nullptr);
    if (!tempStyle.setProperty(CSS_PROP_COLOR, domStr)) {
        return QColor();
    }

    CSSValueImpl *val = tempStyle.getPropertyCSSValue(CSS_PROP_COLOR);
    if (!val || val->cssValueType() != CSSValue::CSS_PRIMITIVE_VALUE) {
        return QColor();
    }

    CSSPrimitiveValueImpl *primVal = static_cast<CSSPrimitiveValueImpl *>(val);

    if (primVal->primitiveType() == CSSPrimitiveValue::CSS_IDENT) {
        return colorForCSSValue(primVal->getIdent());
    }

    if (primVal->primitiveType() != CSSPrimitiveValue::CSS_RGBCOLOR) {
        return QColor();
    }
    return QColor::fromRgba(primVal->getRGBColorValue());
}

static DOMString colorToString(const QColor &color)
{
    const int a = color.alpha();

    if (a == 255) {
        return DOMString(QString::fromLatin1("#%1%2%3")
                         .arg(color.red(),   2, 16, QLatin1Char('0'))
                         .arg(color.green(), 2, 16, QLatin1Char('0'))
                         .arg(color.blue(),  2, 16, QLatin1Char('0')));
    }

    // Use the shortest decimal alpha that still maps back to the same 8-bit value,
    // rather than always printing things like 0.501961.
    QString alphaStr;
    for (int digits = 0; digits <= 3; ++digits) {
        alphaStr = QString::number(a / 255.0, 'f', digits);
        if (qRound(alphaStr.toDouble() * 255.0) == a) {
            break;
        }
    }
    // Ensure we always have a decimal period
    if (!alphaStr.contains(QLatin1Char('.'))) {
        alphaStr += QLatin1String(".0");
    }

    return DOMString(QString::fromLatin1("rgba(%1, %2, %3, %4)")
                     .arg(color.red()).arg(color.green()).arg(color.blue()).arg(alphaStr));
}

//-------

DOM::DOMString CanvasColorImpl::toString() const
{
    return colorToString(color);
}

CanvasColorImpl *CanvasColorImpl::fromString(const DOM::DOMString &str)
{
    QColor cl = colorFromString(str);
    if (!cl.isValid()) {
        return nullptr;
    }
    return new CanvasColorImpl(cl);
}

//-------

CanvasGradientImpl::CanvasGradientImpl(QGradient *newGradient, float innerRadius, bool inverse)
    : gradient(newGradient), innerRadius(innerRadius), inverse(inverse)
{}

// Qt doesn't support two stops at the same position, which <canvas> allows.
// Nudge a colliding stop slightly to emulate it.
static qreal adjustPosition(qreal pos, const QGradientStops &stops)
{
    const qreal smallDiff = 0.00001;
    for (int i = 0; i < stops.size(); ++i) {
        if (!qFuzzyCompare(pos, stops.at(i).first)) {
            continue;
        }
        // Original code had this flag inverted and dereferenced end() when the
        // colliding stop was the last one.
        const bool isLast = (i + 1 == stops.size());
        if (isLast || !qFuzzyCompare(pos + smallDiff, stops.at(i + 1).first)) {
            return qMin(pos + smallDiff, qreal(1.0));
        }
    }
    return pos;
}

void CanvasGradientImpl::addColorStop(float offset, const DOM::DOMString &color, int &exceptionCode)
{
    // ### we may have to handle the "currentColor" KW here. ouch.

    exceptionCode = 0;
    if (isInfArg(offset)) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return;
    }

    //### fuzzy compare (also for alpha)
    if (offset < 0 || offset > 1) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return;
    }

    QColor qcolor = colorFromString(color);
    if (!qcolor.isValid()) {
        exceptionCode = DOMException::SYNTAX_ERR;
        return;
    }

    // Adjust the position of the stop to emulate an inner radius.
    // If the inner radius is larger than the outer, we'll reverse
    // the position of the stop.
    if (gradient->type() == QGradient::RadialGradient) {
        if (inverse) {
            offset = 1.0 - offset;
        }

        offset = innerRadius + offset * (1.0 - innerRadius);
    }

    offset = adjustPosition(offset, gradient->stops());

    gradient->setColorAt(offset, qcolor);
}

CanvasGradientImpl::~CanvasGradientImpl()
{
    delete gradient;
}

QBrush CanvasGradientImpl::toBrush() const
{
    return QBrush(*gradient);
}

//-------

CanvasPatternImpl::CanvasPatternImpl(const QImage &inImg, bool unsafe, bool rx, bool ry):
    img(inImg), repeatX(rx), repeatY(ry), unsafe(unsafe)
{}

QBrush CanvasPatternImpl::toBrush() const
{
    return QBrush(img);
}

QRectF CanvasPatternImpl::clipForRepeat(const QPointF &origin, const QRectF &fillBounds) const
{
    if (repeatX && repeatY) {
        return QRectF();
    }

    if (!repeatX && !repeatY) {
        return QRectF(origin, img.size());
    }

    if (repeatX) {
        return QRectF(fillBounds.x(), origin.y(), fillBounds.width(), img.height());
    }

    // repeatY
    return QRectF(origin.x(), fillBounds.y(), img.width(), fillBounds.height());
}

//-------

CanvasImageDataImpl::CanvasImageDataImpl(unsigned width, unsigned height) : data(width, height, QImage::Format_ARGB32)
{}

CanvasImageDataImpl::CanvasImageDataImpl(const QImage &_data): data(_data)
{}

CanvasImageDataImpl *CanvasImageDataImpl::clone() const
{
    return new CanvasImageDataImpl(data);
}

unsigned CanvasImageDataImpl::width() const
{
    return data.width();
}

unsigned CanvasImageDataImpl::height() const
{
    return data.height();
}

// pixelNum comes straight from script, so it is bounds-checked here: the old
// code did unchecked scanLine()/pixel() access, i.e. out-of-bounds read/write.

QColor CanvasImageDataImpl::pixel(unsigned pixelNum) const
{
    const unsigned w = data.width();
    const unsigned h = data.height();
    if (w == 0 || h == 0 || pixelNum >= w * h) {
        return QColor(0, 0, 0, 0);
    }

    QRgb code = data.pixel(pixelNum % w, pixelNum / w);
    return QColor::fromRgba(code); // keeps alpha; QColor(QRgb) would drop it
}

void CanvasImageDataImpl::setPixel(unsigned pixelNum, const QColor &val)
{
    const unsigned w = data.width();
    const unsigned h = data.height();
    if (w == 0 || h == 0 || pixelNum >= w * h) {
        return;
    }

    data.setPixel(pixelNum % w, pixelNum / w, val.rgba());
}

void CanvasImageDataImpl::setComponent(unsigned pixelNum, int component,
                                       int value)
{
    const unsigned w = data.width();
    const unsigned h = data.height();
    if (w == 0 || h == 0 || pixelNum >= w * h) {
        return;
    }

    value = qBound(0, value, 255); // Uint8ClampedArray semantics

    const int x = pixelNum % w;
    const int y = pixelNum / w;
    // ### could avoid inherent QImage::detach() by a const cast
    QRgb *rgb = reinterpret_cast<QRgb *>(data.scanLine(y) + 4 * x);

    switch (component) {
    case 0: //Red
        *rgb = qRgba(value, qGreen(*rgb), qBlue(*rgb), qAlpha(*rgb));
        break;
    case 1: //Green
        *rgb = qRgba(qRed(*rgb), value, qBlue(*rgb), qAlpha(*rgb));
        break;
    case 2: //Blue
        *rgb = qRgba(qRed(*rgb), qGreen(*rgb), value, qAlpha(*rgb));
        break;
    case 3: //Alpha
    default:
        *rgb = qRgba(qRed(*rgb), qGreen(*rgb), qBlue(*rgb), value);
        break;
    }
}

//-------

// The canvas element can already be gone while script still holds the context,
// so never dereference canvas() blindly.
#define MARK_CANVAS_UNSAFE() \
    do { if (HTMLCanvasElementImpl *c__ = canvas()) c__->markUnsafe(); } while (0)

void CanvasContext2DImpl::setStrokeStyle(CanvasStyleBaseImpl *strokeStyle)
{
    if (!strokeStyle) {
        return;
    }
    if (strokeStyle->isUnsafe()) {
        MARK_CANVAS_UNSAFE();
    }

    activeState().strokeStyle = strokeStyle;
    dirty |= DrtStroke;
}

CanvasStyleBaseImpl *CanvasContext2DImpl::strokeStyle() const
{
    return activeState().strokeStyle.get();
}

void CanvasContext2DImpl::setFillStyle(CanvasStyleBaseImpl *fillStyle)
{
    if (!fillStyle) {
        return;
    }
    if (fillStyle->isUnsafe()) {
        MARK_CANVAS_UNSAFE();
    }

    activeState().fillStyle = fillStyle;
    dirty |= DrtFill;
}

CanvasStyleBaseImpl *CanvasContext2DImpl::fillStyle() const
{
    return activeState().fillStyle.get();
}

CanvasGradientImpl *CanvasContext2DImpl::createLinearGradient(float x0, float y0, float x1, float y1) const
{
    QLinearGradient *grad = new QLinearGradient(x0, y0, x1, y1);
    return new CanvasGradientImpl(grad);
}

CanvasGradientImpl *CanvasContext2DImpl::createRadialGradient(float x0, float y0, float r0,
        float x1, float y1, float r1,
        int &exceptionCode) const
{
    exceptionCode = 0;
    //### fuzzy
    if (anyNonFinite(x0, y0, r0, x1, y1, r1) || r0 < 0.0f || r1 < 0.0f) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return nullptr;
    }

    QPointF center, focalPoint;
    float radius, innerRadius;
    bool inverse;

    // Use the larger of the two radii as the radius in the QGradient.
    // The gradient is always supposed to move from r0 to r1, so if r0 is
    // larger than r1, we'll use r0 as the radius and reverse the direction
    // of the gradient by inverting the positions of the color stops.
    // innerRadius is a percentage of the outer radius.
    if (r1 > r0) {
        center      = QPointF(x1, y1);
        focalPoint  = QPointF(x0, y0);
        radius      = r1;
        innerRadius = (r1 > 0.0f ? r0 / r1 : 0.0f);
        inverse     = false;
    } else {
        center      = QPointF(x0, y0);
        focalPoint  = QPointF(x1, y1);
        radius      = r0;
        innerRadius = (r0 > 0.0f ? r1 / r0 : 0.0f);
        inverse     = true;
    }

    QGradient *gradient = new QRadialGradient(center, radius, focalPoint);
    return new CanvasGradientImpl(gradient, innerRadius, inverse);
}

CanvasPatternImpl *CanvasContext2DImpl::createPattern(ElementImpl *pat, const DOMString &rpt,
        int &exceptionCode) const
{
    exceptionCode = 0;

    // Decode repetition..
    bool repeatX;
    bool repeatY;

    if (rpt == "repeat" || rpt.isEmpty()) {
        repeatX = true;
        repeatY = true;
    } else if (rpt == "repeat-x") {
        repeatX = true;
        repeatY = false;
    } else if (rpt == "repeat-y") {
        repeatX = false;
        repeatY = true;
    } else if (rpt == "no-repeat") {
        repeatX = false;
        repeatY = false;
    } else {
        exceptionCode = DOMException::SYNTAX_ERR;
        return nullptr;
    }

    bool unsafe;
    QImage pic = extractImage(pat, exceptionCode, unsafe);
    if (exceptionCode) {
        return nullptr;
    }

    return new CanvasPatternImpl(pic, unsafe, repeatX, repeatY);
}

// Pen style ops
//
float CanvasContext2DImpl::lineWidth() const
{
    return activeState().lineWidth;
}

void CanvasContext2DImpl::setLineWidth(float newLW)
{
    if (isInfArg(newLW) || newLW <= 0.0f) { // NaN used to slip through "<= 0"
        return;
    }
    activeState().lineWidth = newLW;
    dirty |= DrtStroke;
}

void CanvasContext2DImpl::setLineDash(const QVector<qreal> &segments)
{
    qreal sum = 0;
    for (int i = 0; i < segments.size(); ++i) {
        const qreal v = segments.at(i);
        if (isInfArg(float(v)) || v < 0) {
            return; // spec: invalid list is ignored
        }
        sum += v;
    }

    QVector<qreal> dash = segments;
    if (dash.size() % 2) {
        dash += segments; // odd count: repeat the list
    }
    if (sum <= 0) {
        dash.clear(); // all zeros == solid line (and would hang the stroker)
    }
    activeState().lineDash = dash;
}

QVector<qreal> CanvasContext2DImpl::lineDash() const
{
    return activeState().lineDash;
}

float CanvasContext2DImpl::lineDashOffset() const
{
    return activeState().lineDashOffset;
}

void CanvasContext2DImpl::setLineDashOffset(float offset)
{
    if (isInfArg(offset)) {
        return;
    }
    activeState().lineDashOffset = offset;
}

static const IDTranslator<QString, Qt::PenCapStyle, const char *>::Info penCapTranslatorTable[] = {
    {"round", Qt::RoundCap},
    {"square", Qt::SquareCap},
    {"butt", Qt::FlatCap},
    {nullptr, (Qt::PenCapStyle)0}
};

MAKE_TRANSLATOR(penCapTranslator, QString, Qt::PenCapStyle, const char *, penCapTranslatorTable)

DOMString CanvasContext2DImpl::lineCap() const
{
    return penCapTranslator()->toLeft(activeState().lineCap);
}

void CanvasContext2DImpl::setLineCap(const DOM::DOMString &cap)
{
    QString capStr = cap.string();
    if (!penCapTranslator()->hasLeft(capStr)) {
        return;
    }
    activeState().lineCap = penCapTranslator()->toRight(capStr);
    dirty |= DrtStroke;
}

static const IDTranslator<QString, Qt::PenJoinStyle, const char *>::Info penJoinTranslatorTable[] = {
    {"round", Qt::RoundJoin},
    {"miter", Qt::SvgMiterJoin},
    {"bevel", Qt::BevelJoin},
    {nullptr, (Qt::PenJoinStyle)0}
};

MAKE_TRANSLATOR(penJoinTranslator, QString, Qt::PenJoinStyle, const char *, penJoinTranslatorTable)

DOMString CanvasContext2DImpl::lineJoin() const
{
    return penJoinTranslator()->toLeft(activeState().lineJoin);
}

void CanvasContext2DImpl::setLineJoin(const DOM::DOMString &join)
{
    QString joinStr = join.string();
    if (!penJoinTranslator()->hasLeft(joinStr)) {
        return;
    }
    activeState().lineJoin = penJoinTranslator()->toRight(joinStr);
    dirty |= DrtStroke;
}

float CanvasContext2DImpl::miterLimit() const
{
    return activeState().miterLimit;
}

void CanvasContext2DImpl::setMiterLimit(float newML)
{
    if (isInfArg(newML) || newML <= 0.0f) {
        return;
    }
    activeState().miterLimit = newML;
    dirty |= DrtStroke;
}

// Shadow settings
//
float CanvasContext2DImpl::shadowOffsetX() const
{
    return activeState().shadowOffsetX;
}

void  CanvasContext2DImpl::setShadowOffsetX(float newOX)
{
    if (isInfArg(newOX)) {
        return;
    }
    activeState().shadowOffsetX = newOX;
}

float CanvasContext2DImpl::shadowOffsetY() const
{
    return activeState().shadowOffsetY;
}

void  CanvasContext2DImpl::setShadowOffsetY(float newOY)
{
    if (isInfArg(newOY)) {
        return;
    }
    activeState().shadowOffsetY = newOY;
}

float CanvasContext2DImpl::shadowBlur() const
{
    return activeState().shadowBlur;
}

void  CanvasContext2DImpl::setShadowBlur(float newBlur)
{
    if (isInfArg(newBlur) || newBlur < 0) {
        return;
    }

    activeState().shadowBlur = newBlur;
}

DOMString CanvasContext2DImpl::shadowColor() const
{
    return colorToString(activeState().shadowColor);
}

void CanvasContext2DImpl::setShadowColor(const DOMString &newColor)
{
    // This not specified, it seems, but I presume setting
    // and invalid color does not change the state
    QColor cl = colorFromString(newColor);
    if (cl.isValid()) {
        activeState().shadowColor = cl;
    }
}

// Rectangle ops
//
void CanvasContext2DImpl::clearRect(float x, float y, float w, float h, int &exceptionCode)
{
    exceptionCode = 0;
    if (anyNonFinite(x, y, w, h) || w == 0.0f || h == 0.0f) {
        return;
    }

    QPainter *p = acquirePainter();

    // The old version filled an untransformed rect, so clearRect() ignored the
    // current transform, and a partial globalAlpha only half-cleared.
    // save()/restore() puts the painter back exactly as it was, so no dirty
    // flags need to be touched.
    p->save();
    p->setOpacity(1.0);
    p->setCompositionMode(QPainter::CompositionMode_Source);
    p->fillPath(transformedRectPath(QRectF(x, y, w, h), activeState().transform), Qt::transparent);
    p->restore();
}

void CanvasContext2DImpl::fillRect(float x, float y, float w, float h, int &exceptionCode)
{
    exceptionCode = 0;
    if (anyNonFinite(x, y, w, h) || w == 0.0f || h == 0.0f) {
        return;
    }

    QPainter *p = acquirePainter();
    drawPath(p, transformedRectPath(QRectF(x, y, w, h), activeState().transform), DrawFill);
}

void CanvasContext2DImpl::strokeRect(float x, float y, float w, float h, int &exceptionCode)
{
    exceptionCode = 0;
    if (anyNonFinite(x, y, w, h) || (w == 0.0f && h == 0.0f)) {
        return;
    }

    QPainter *p = acquirePainter();
    drawPath(p, transformedRectPath(QRectF(x, y, w, h), activeState().transform), DrawStroke);
}

inline bool CanvasContext2DImpl::isPathEmpty() const
{
    // For an explanation of this, see the comment in beginPath()
    return emptyPath;
}

// Path ops
//
void CanvasContext2DImpl::beginPath()
{
    path = QPainterPath();
    path.setFillRule(Qt::WindingFill);

    // QPainterPath always contains an initial MoveTo element to (0, 0), and there is
    // no way to tell.
    // We used to insert a Inf/Inf element to tell if its empty. But that no longer
    // works with Qt newer than 2011-01-21
    // https://code.qt.io/cgit/qt/qt.git/commit/?id=972fcb6de69fb7ed3ae8147498ceb5d2ac79f057
    // Now go with a extra bool to check if its really empty.
    emptyPath = true;
}

void CanvasContext2DImpl::closePath()
{
    path.closeSubpath();
}

void CanvasContext2DImpl::moveTo(float x, float y)
{
    if (anyNonFinite(x, y)) {
        return;
    }

    path.moveTo(mapToDevice(x, y));
    emptyPath = false;
}

void CanvasContext2DImpl::lineTo(float x, float y)
{
    if (anyNonFinite(x, y)) {
        return;
    }

    if (isPathEmpty()) {
        // Spec: with no subpath, lineTo() behaves like moveTo().
        // (This used to be silently dropped.)
        path.moveTo(mapToDevice(x, y));
        emptyPath = false;
        return;
    }

    path.lineTo(mapToDevice(x, y));
}

void CanvasContext2DImpl::quadraticCurveTo(float cpx, float cpy, float x, float y)
{
    if (anyNonFinite(cpx, cpy, x, y)) {
        return;
    }

    if (isPathEmpty()) {
        path.moveTo(mapToDevice(cpx, cpy));
        emptyPath = false;
    }

    path.quadTo(mapToDevice(cpx, cpy), mapToDevice(x, y));
}

void CanvasContext2DImpl::bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y)
{
    if (anyNonFinite(cp1x, cp1y, cp2x, cp2y, x, y)) {
        return;
    }

    if (isPathEmpty()) {
        path.moveTo(mapToDevice(cp1x, cp1y));
        emptyPath = false;
    }

    path.cubicTo(mapToDevice(cp1x, cp1y), mapToDevice(cp2x, cp2y), mapToDevice(x, y));
}

void CanvasContext2DImpl::rect(float x, float y, float w, float h, int &exceptionCode)
{
    exceptionCode = 0;
    if (anyNonFinite(x, y, w, h)) {
        return;
    }

    path.addPolygon(activeState().transform.map(QPolygonF(QRectF(x, y, w, h))));
    path.closeSubpath();
    emptyPath = false; // was missing: following lineTo()/arc() thought the path was empty
}

inline bool CanvasContext2DImpl::needsShadow() const
{
    // A shadow with no blur and no offset is invisible, so skip the (expensive)
    // offscreen shadow pass in that case.
    const PaintState &s = activeState();
    return s.shadowColor.alpha() > 0 &&
           (s.shadowBlur > 0.0f || s.shadowOffsetX != 0.0f || s.shadowOffsetY != 0.0f);
}

QPainterPath CanvasContext2DImpl::clipForPatternRepeat(QPainter *p, PathPaintOp op) const
{
    const CanvasStyleBaseImpl *style = op == DrawFill ?
                                       activeState().fillStyle.get() : activeState().strokeStyle.get();

    if (style->type() != CanvasStyleBaseImpl::Pattern) {
        return QPainterPath();
    }

    const CanvasPatternImpl *pattern = static_cast<const CanvasPatternImpl *>(style);
    const QTransform &ctm = activeState().transform;
    if (!ctm.isInvertible()) {
        return QPainterPath();
    }

    const QRectF fillBounds = ctm.inverted().mapRect(QRectF(QPointF(), canvasImage->size()));
    const QRectF clipRect = pattern->clipForRepeat(p->brushOrigin(), fillBounds);

    if (clipRect.isEmpty()) {
        return QPainterPath();
    }

    QPainterPath path;
    path.addRect(clipRect);
    return path * ctm;
}

QPainterPath CanvasContext2DImpl::strokeOutline(const QPainterPath &devicePath) const
{
    const PaintState &state = activeState();
    if (!state.transform.isInvertible()) {
        return QPainterPath(); // degenerate transform: nothing to draw
    }

    QPainterPathStroker stroker;
    stroker.setCapStyle(state.lineCap);
    stroker.setJoinStyle(state.lineJoin);
    stroker.setMiterLimit(state.miterLimit);
    stroker.setWidth(state.lineWidth);

    if (!state.lineDash.isEmpty()) {
        // Qt measures dashes in multiples of the pen width; canvas uses user units.
        QVector<qreal> pattern;
        pattern.reserve(state.lineDash.size());
        for (int i = 0; i < state.lineDash.size(); ++i) {
            pattern.append(state.lineDash.at(i) / state.lineWidth);
        }
        stroker.setDashPattern(pattern);
        stroker.setDashOffset(state.lineDashOffset / state.lineWidth);
    }

    if (state.transform.isIdentity()) {
        return stroker.createStroke(devicePath);
    }
    // Stroke in user space so non-uniform scales distort the pen, as specified.
    return stroker.createStroke(devicePath * state.transform.inverted()) * state.transform;
}

void CanvasContext2DImpl::drawPath(QPainter *p, const QPainterPath &path, const PathPaintOp op) const
{
    const PaintState &state = activeState();
    QPainterPath fillPath;
    QBrush brush;

    if (state.infinityTransform) {
        return;
    }

    switch (op) {
    case DrawStroke:
        brush = p->pen().brush();
        fillPath = strokeOutline(path);
        break;

    case DrawFill:
        brush = p->brush();
        fillPath = path;
        break;
    }

    brush.setTransform(state.transform);

    p->save();
    p->setPen(Qt::NoPen);
    p->setBrush(brush);

    if (needsShadow()) {
        drawPathWithShadow(p, fillPath, op);
    } else {
        const QPainterPath repeatClip = clipForPatternRepeat(p, op);
        if (!repeatClip.isEmpty()) {
            p->setClipPath(repeatClip, Qt::IntersectClip);
        }

        p->drawPath(fillPath);
    }
    p->restore();
}

void CanvasContext2DImpl::drawPathWithShadow(QPainter *p, const QPainterPath &path, PathPaintOp op, PaintFlags flags) const
{
    const PaintState &state = activeState();
    float radius = shadowBlur();

    // This seems to produce a shadow that's a fairly close approximation
    // to the shadows rendered by CoreGraphics.
    if (radius > 7) {
        radius = qMin(7 + std::pow(float(radius - 7.0), float(.7)), float(127.0));
    }

    const qreal offset = radius * 2;
    const QPainterPath repeatClip = (flags & NotUsingCanvasPattern) ?
                                    QPainterPath() : clipForPatternRepeat(p, op);

    QRect shapeBounds;
    if (!repeatClip.isEmpty()) {
        shapeBounds = path.intersected(repeatClip).controlPointRect().toAlignedRect();
    } else {
        shapeBounds = path.controlPointRect().toAlignedRect();
    }

    QRect clipRect;
    if (state.clipping) {
        clipRect = state.clipPath.controlPointRect().toAlignedRect();
        clipRect &= QRect(QPoint(), canvasImage->size());
    } else {
        clipRect = QRect(QPoint(), canvasImage->size());
    }

    // Shadow offsets are in device space and ignore the CTM, per spec.
    const QRect shadowRect = shapeBounds.translated(safeRound(shadowOffsetX()), safeRound(shadowOffsetY()))
                             .adjusted(-offset, -offset, offset, offset) &
                             clipRect.adjusted(-offset, -offset, offset, offset);

    const QRect shapeRect = QRect(shapeBounds & clipRect) |
                            (shadowRect.translated(-safeRound(shadowOffsetX()), -safeRound(shadowOffsetY())) & shapeBounds);

    if (!shapeRect.isValid()) {
        return;
    }

    QPainter painter;

    // Create the image for the original shape
    QImage shape(shapeRect.size(), QImage::Format_ARGB32_Premultiplied);
    shape.fill(0);

    // Draw the shape
    painter.begin(&shape);
    painter.setRenderHints(p->renderHints());
    painter.setBrushOrigin(p->brushOrigin());
    painter.setBrush(p->brush());
    painter.setPen(Qt::NoPen);
    painter.translate(-shapeRect.x(), -shapeRect.y());
    if (!repeatClip.isEmpty()) {
        painter.setClipPath(repeatClip);
    }
    painter.drawPath(path);
    painter.end();

    // Create the shadow image and draw the original image on it
    if (shadowRect.isValid()) {
        QImage shadow(shadowRect.size(), QImage::Format_ARGB32_Premultiplied);
        shadow.fill(0);

        painter.begin(&shadow);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.translate(-shadowRect.x(), -shadowRect.y());
        painter.drawImage(shapeRect.x() + shadowOffsetX(),
                          shapeRect.y() + shadowOffsetY(), shape);
        painter.end();

        // Blur the alpha channel
        ImageFilter::shadowBlur(shadow, radius, state.shadowColor);

        // Draw the shadow on the canvas
        p->drawImage(shadowRect.topLeft(), shadow);
    }

    // Composite the original image over the shadow.
    p->drawImage(shapeRect.topLeft(), shape);
}

void CanvasContext2DImpl::fill()
{
    QPainter *p = acquirePainter();
    drawPath(p, path, DrawFill);
}

void CanvasContext2DImpl::stroke()
{
    QPainter *p = acquirePainter();
    drawPath(p, path, DrawStroke);
}

void CanvasContext2DImpl::clip()
{
    PaintState &state = activeState();
    QPainterPath pathCopy = path;
    pathCopy.closeSubpath();

    if (state.clipping) {
        state.clipPath = state.clipPath.intersected(pathCopy);
    } else {
        state.clipPath = pathCopy;
    }

    state.clipPath.setFillRule(Qt::WindingFill);
    state.clipping = true;
    dirty |= DrtClip;
}

bool CanvasContext2DImpl::isPointInPath(float x, float y) const
{
    if (anyNonFinite(x, y)) {
        return false;
    }

    // The path is stored in device space, so the query point (user space) has to
    // go through the CTM too. Previously it was compared untransformed.
    return path.contains(mapToDevice(x, y));
}

bool CanvasContext2DImpl::isPointInStroke(float x, float y) const
{
    if (anyNonFinite(x, y) || path.isEmpty()) {
        return false;
    }
    return strokeOutline(path).contains(mapToDevice(x, y));
}

void CanvasContext2DImpl::arcTo(float x1, float y1, float x2, float y2, float radius, int &exceptionCode)
{
    exceptionCode = 0;

    if (anyNonFinite(x1, y1, x2, y2, radius)) {
        return;
    }

    if (radius < 0) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return;
    }

    if (isPathEmpty()) {
        moveTo(x1, y1);
    }
    emptyPath = false;

    // Spec: a zero radius is just a straight line to (x1, y1). (Used to throw.)
    if (radius == 0) {
        path.lineTo(mapToDevice(x1, y1));
        return;
    }

    QLineF line1(QPointF(x1, y1), mapToUser(path.currentPosition()));
    QLineF line2(QPointF(x1, y1), QPointF(x2, y2));

    // If the first line is a point, we'll do nothing.
    if (line1.p1() == line1.p2()) {
        return;
    }

    // If the second line is a point, we'll add a line segment to (x1, y1).
    if (line2.p1() == line2.p2()) {
        path.lineTo(mapToDevice(x1, y1));
        return;
    }

    float angle1 = std::atan2(line1.dy(), line1.dx());
    float angle2 = std::atan2(line2.dy(), line2.dx());

    // The smallest angle between the lines
    float theta = angle2 - angle1;
    if (theta < -M_PI) {
        theta = (2 * M_PI + theta);
    } else if (theta > M_PI) {
        theta = -(2 * M_PI - theta);
    }

    // If the angle between the lines is 180 degrees (or 0), the span of the arc
    // becomes zero, causing the tangent points to converge to the same point at (x1, y1).
    if (qFuzzyCompare(qAbs(theta), float(M_PI)) || qFuzzyIsNull(theta)) {
        path.lineTo(mapToDevice(x1, y1));
        return;
    }

    // The length of the hypotenuse of the right triangle formed by the points
    // (x1, y1), the center point of the circle, and either of the two tangent points.
    float h = radius / std::sin(qAbs(theta / 2.0));

    // The distance from (x1, y1) to the tangent points on line1 and line2.
    float tDist = std::cos(theta / 2.0) * h;

    // As theta approaches 0, the distance to the two tangent points approach infinity.
    // If we exceeded the data type limit, draw a long line toward the first tangent point.
    // This matches CoreGraphics and Postscript behavior.
    if (KJS::isInf(h) || KJS::isInf(tDist)) {
        QPointF point(line1.p2().x() + std::cos(angle1) * 1e10,
                      line1.p2().y() + std::sin(angle1) * 1e10);
        path.lineTo(mapToDevice(point));
        return;
    }

    // The center point of the circle
    float angle = angle1 + theta / 2.0;
    QPointF centerPoint(x1 + std::cos(angle) * h, y1 + std::sin(angle) * h);

    // Note that we don't check if the lines are long enough for the circle to actually
    // tangent them; like CoreGraphics and Postscript, we treat the points as points on
    // two infinitely long lines that intersect one another at (x1, y1).
    float startAngle = theta < 0 ? angle1 + M_PI_2 : angle1 - M_PI_2;
    float endAngle   = theta < 0 ? angle2 - M_PI_2 : angle2 + M_PI_2;
    bool counterClockWise = theta > 0;

    int dummy; // Exception code from arc()
    arc(centerPoint.x(), centerPoint.y(), radius, startAngle, endAngle, counterClockWise, dummy);
}

void CanvasContext2DImpl::arc(float x, float y, float radius, float startAngle, float endAngle,
                              bool counterClockWise, int &exceptionCode)
{
    exceptionCode = 0;

    if (anyNonFinite(x, y, radius, startAngle, endAngle)) {
        return;
    }

    if (radius < 0) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return;
    }

    if (radius == 0) {
        // Degenerate arc: just a point / line to it.
        if (isPathEmpty()) {
            path.moveTo(mapToDevice(x, y));
        } else {
            path.lineTo(mapToDevice(x, y));
        }
        emptyPath = false;
        return;
    }

    const QRectF rect(x - radius, y - radius, radius * 2, radius * 2);
    float sweepLength = -degrees(endAngle - startAngle);
    startAngle = -degrees(startAngle);

    if (counterClockWise && (sweepLength < 0 || sweepLength > 360)) {
        sweepLength = 360 + std::fmod(sweepLength, float(360.0));
        if (qFuzzyCompare(sweepLength + 1, 1)) {
            sweepLength = 360;
        }
    } else if (!counterClockWise && (sweepLength > 0 || sweepLength < -360)) {
        sweepLength = -(360 - std::fmod(sweepLength, float(360.0)));
        if (qFuzzyCompare(sweepLength + 1, 1)) {
            sweepLength = -360; // clockwise full circle (was +360, i.e. wrong direction)
        }
    }

    QPainterPath arcPath;
    arcPath.arcMoveTo(rect, startAngle);
    arcPath.arcTo(rect, startAngle, sweepLength);

    // When drawing the arc, Safari will loop around the circle several times if
    // the sweep length is greater than 360 degrees, leaving the current position
    // in the path at endAngle. QPainterPath::arcTo() will stop when it reaches
    // 360 degrees, thus leaving the current position at that point. To match
    // Safari behavior, we call QPainterPath::arcTo() twice in this case, to make
    // the arc continue to the intended end point. Adding a MoveTo element will
    // not suffice, since this will not produce correct results if additional
    // elements are added to the path before it is stroked or filled.
    if (sweepLength > 360.0 || sweepLength < -360.0) {
        if (sweepLength < 0) {
            sweepLength += 360.0;
            startAngle -= 360.0;
        } else {
            sweepLength -= 360.0;
            startAngle += 360.0;
        }

        arcPath.arcTo(rect, startAngle, sweepLength);
    }

    // Add the transformed arc to the path
    if (isPathEmpty()) {
        path.addPath(arcPath * activeState().transform);
    } else {
        if (path.elementAt(path.elementCount() - 1).type != QPainterPath::MoveToElement) {
            path.connectPath(arcPath * activeState().transform);
        } else {
            // ### This is needed to work around buggy behavior in QPainterPath::connectPath()
            //     when the last element in the path being added to is a MoveToElement.
            arcPath = arcPath * activeState().transform;
            path.lineTo(arcPath.elementAt(0));

            if (arcPath.elementCount() > 1) {
                for (int i = 1; i + 2 < arcPath.elementCount(); i += 3) { // bounds-safe step
                    path.cubicTo(arcPath.elementAt(i), arcPath.elementAt(i + 1),
                                 arcPath.elementAt(i + 2));
                }
            }
        }
    }
    emptyPath = false;
}

void CanvasContext2DImpl::ellipse(float x, float y, float radiusX, float radiusY, float rotation,
                                  float startAngle, float endAngle, bool counterClockWise,
                                  int &exceptionCode)
{
    exceptionCode = 0;

    if (anyNonFinite(x, y, radiusX, radiusY, rotation, startAngle, endAngle)) {
        return;
    }

    if (radiusX < 0 || radiusY < 0) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return;
    }

    if (radiusX == 0 || radiusY == 0) {
        // Degenerate: a point / line to it, like a zero-radius arc.
        if (isPathEmpty()) {
            path.moveTo(mapToDevice(x, y));
        } else {
            path.lineTo(mapToDevice(x, y));
        }
        emptyPath = false;
        return;
    }

    // Draw a unit-circle arc under a temporarily extended CTM, so the ellipse
    // is exact (no bezier-approximation of scaling) and reuses arc()'s logic.
    PaintState &state = activeState();
    const QTransform saved = state.transform;
    state.transform.translate(x, y);
    state.transform.rotateRadians(rotation);
    state.transform.scale(radiusX, radiusY);

    int dummy;
    arc(0, 0, 1, startAngle, endAngle, counterClockWise, dummy);

    activeState().transform = saved;
}

void CanvasContext2DImpl::drawImage(QPainter *p, const QRectF &dstRect, const QImage &image, const QRectF &srcRect) const
{
    if (activeState().infinityTransform || image.isNull()) {
        return;
    }

    if (!needsShadow()) {
        p->setTransform(activeState().transform);
        p->drawImage(dstRect, image, srcRect);
        p->resetTransform();
        return;
    }

    float xscale = dstRect.width() / srcRect.width();
    float yscale = dstRect.height() / srcRect.height();
    float dx = dstRect.x() - srcRect.x() * xscale;
    float dy = dstRect.y() - srcRect.y() * yscale;

    QTransform transform;
    transform.translate(dx, dy);
    transform.scale(xscale, yscale);

    QBrush brush(image);
    brush.setTransform(transform * activeState().transform);

    QPainterPath path;
    path.addRect(dstRect);
    path = path * activeState().transform;

    p->save();
    p->setBrush(brush);
    p->setPen(Qt::NoPen);
    drawPathWithShadow(p, path, DrawFill, NotUsingCanvasPattern);
    p->restore();
}

// Image stuff
void CanvasContext2DImpl::drawImage(ElementImpl *image, float dx, float dy, int &exceptionCode)
{
    exceptionCode = 0;
    if (anyNonFinite(dx, dy)) {
        return;
    }

    bool unsafe;
    QImage img = extractImage(image, exceptionCode, unsafe);
    if (unsafe) {
        MARK_CANVAS_UNSAFE();
    }
    if (exceptionCode || img.isNull()) {
        return;
    }

    QPainter *p = acquirePainter();
    drawImage(p, QRectF(dx, dy, img.width(), img.height()), img, img.rect());
}

void CanvasContext2DImpl::drawImage(ElementImpl *image, float dx, float dy, float dw, float dh,
                                    int &exceptionCode)
{
    //### do we need DoS protection here?
    exceptionCode = 0;
    if (anyNonFinite(dx, dy, dw, dh)) {
        return;
    }

    bool unsafe;
    QImage img = extractImage(image, exceptionCode, unsafe);
    if (unsafe) {
        MARK_CANVAS_UNSAFE();
    }
    if (exceptionCode || img.isNull()) {
        return;
    }

    if (dw < 0 || dh < 0) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return;
    }

    if (qFuzzyIsNull(dw) || qFuzzyIsNull(dh)) {
        return;
    }

    QPainter *p = acquirePainter();
    drawImage(p, QRectF(dx, dy, dw, dh), img, img.rect());
}

void CanvasContext2DImpl::drawImage(ElementImpl *image,
                                    float sx, float sy, float sw, float sh,
                                    float dx, float dy, float dw, float dh,
                                    int &exceptionCode)
{
    //### do we need DoS protection here?
    exceptionCode = 0;
    if (anyNonFinite(sx, sy, sw, sh, dx, dy, dw, dh)) {
        return;
    }

    bool unsafe;
    QImage img = extractImage(image, exceptionCode, unsafe);
    if (unsafe) {
        MARK_CANVAS_UNSAFE();
    }
    if (exceptionCode || img.isNull()) {
        return;
    }

    if (sx < 0 || sy < 0 || sw < 0 || sh < 0 || dw < 0 || dh < 0 ||
            sx + sw > img.width() || sy + sh > img.height()) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return;
    }

    if (qFuzzyIsNull(sw) || qFuzzyIsNull(sh) ||
            qFuzzyIsNull(dw) || qFuzzyIsNull(dh)) {
        return;
    }

    QPainter *p = acquirePainter();
    drawImage(p, QRectF(dx, dy, dw, dh), img, QRectF(sx, sy, sw, sh));
}

// Pixel stuff.
//
// All three entry points now set exceptionCode on every path (it used to be left
// uninitialised on success), reject non-finite values, and bound allocations.

CanvasImageDataImpl *CanvasContext2DImpl::getImageData(float sx, float sy, float sw, float sh, int &exceptionCode)
{
    exceptionCode = 0;

    if (anyNonFinite(sx, sy, sw, sh)) {
        exceptionCode = DOMException::NOT_SUPPORTED_ERR;
        return nullptr;
    }

    HTMLCanvasElementImpl *el = canvas();
    if (el && el->isUnsafe()) {
        exceptionCode = DOMException::INVALID_ACCESS_ERR;
        return nullptr;
    }

    int w = safeRound(sw);
    int h = safeRound(sh);

    if (w <= 0 || h <= 0) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return nullptr;
    }

    if (!khtmlImLoad::ImageManager::isAcceptableSize(unsigned(w), unsigned(h))) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return nullptr;
    }

    int x = safeRound(sx);
    int y = safeRound(sy);

    CanvasImageDataImpl *id = new CanvasImageDataImpl(w, h);
    id->data.fill(Qt::transparent);

    // Clip the source rect against the canvas bitmap (not the element, which may be gone).
    QRect srcRect = QRect(x, y, w, h);
    QRect clpRect = srcRect & QRect(QPoint(), canvasImage->size());
    if (!clpRect.isEmpty()) {
        // Flush our data..
        syncBackBuffer();

        QPainter p(&id->data);
        p.setCompositionMode(QPainter::CompositionMode_Source);

        // Copy it over..
        QImage *backBuffer = canvasImage->qimage();
        p.drawImage(clpRect.topLeft() - srcRect.topLeft(), *backBuffer, clpRect);
        p.end();
    }

    return id;
}

void CanvasContext2DImpl::putImageData(CanvasImageDataImpl *id, float dx, float dy, int &exceptionCode)
{
    if (!id) {
        exceptionCode = DOMException::TYPE_MISMATCH_ERR;
        return;
    }
    putImageData(id, dx, dy, 0, 0, id->width(), id->height(), exceptionCode);
}

void CanvasContext2DImpl::putImageData(CanvasImageDataImpl *id, float dx, float dy,
                                       float dirtyX, float dirtyY, float dirtyWidth, float dirtyHeight,
                                       int &exceptionCode)
{
    exceptionCode = 0;

    if (!id) {
        exceptionCode = DOMException::TYPE_MISMATCH_ERR;
        return;
    }

    if (anyNonFinite(dx, dy, dirtyX, dirtyY, dirtyWidth, dirtyHeight)) {
        exceptionCode = DOMException::NOT_SUPPORTED_ERR;
        return;
    }

    // Negative dirty sizes flip the rectangle; then clamp it to the image.
    if (dirtyWidth < 0) {
        dirtyX += dirtyWidth;
        dirtyWidth = -dirtyWidth;
    }
    if (dirtyHeight < 0) {
        dirtyY += dirtyHeight;
        dirtyHeight = -dirtyHeight;
    }
    const QRect src = QRect(safeRound(dirtyX), safeRound(dirtyY),
                            safeRound(dirtyWidth), safeRound(dirtyHeight)) & id->data.rect();
    if (src.isEmpty()) {
        return;
    }

    // Flush any previous changes
    syncBackBuffer();

    // We use our own painter here since we should not be affected by clipping, etc.
    // Hence we need to mark ourselves dirty, too
    needRendererUpdate();
    QPainter p(canvasImage->qimage());
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(QPoint(safeRound(dx) + src.x(), safeRound(dy) + src.y()), id->data, src);
    p.end();
}

CanvasImageDataImpl *CanvasContext2DImpl::createImageData(float sw, float sh, int &exceptionCode)
{
    exceptionCode = 0;

    if (anyNonFinite(sw, sh)) {
        exceptionCode = DOMException::NOT_SUPPORTED_ERR;
        return nullptr;
    }

    int w = safeRound(qAbs(sw));
    int h = safeRound(qAbs(sh));

    if (w == 0 || h == 0) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return nullptr;
    }

    // DoS protection: script could previously ask for a multi-gigabyte buffer.
    if (!khtmlImLoad::ImageManager::isAcceptableSize(unsigned(w), unsigned(h))) {
        exceptionCode = DOMException::INDEX_SIZE_ERR;
        return nullptr;
    }

    CanvasImageDataImpl *id = new CanvasImageDataImpl(w, h);
    id->data.fill(Qt::transparent);

    return id;
}

#undef MARK_CANVAS_UNSAFE

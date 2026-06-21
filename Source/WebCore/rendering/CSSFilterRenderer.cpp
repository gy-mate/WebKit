/*
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2013 Google Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "CSSFilterRenderer.h"

#include "ElementChildIteratorInlines.h"
#include "FEComponentTransfer.h"
#include "FilterOperations.h"
#include "Logging.h"
#include "ReferencedSVGResources.h"
#include "RenderElement.h"
#include "RenderElementInlines.h"
#include "RenderObjectInlines.h"
#include "SVGComponentTransferFunctionElement.h"
#include "SVGComponentTransferFunctionElementInlines.h"
#include "SVGElementTypeHelpers.h"
#include "SVGFEComponentTransferElement.h"
#include "SVGFilterElement.h"
#include "SVGFilterPrimitiveStandardAttributes.h"
#include "SVGFilterRenderer.h"
#include "SVGNames.h"
#include "SourceGraphic.h"
#include "StyleFilter.h"
#include "StylePrimitiveNumericTypes+Logging.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CSSFilterRenderer);

RefPtr<CSSFilterRenderer> CSSFilterRenderer::create(RenderElement& renderer, const Style::Filter& filter, const FilterGeometry& geometry, OptionSet<FilterRenderingMode> preferredRenderingModes, OptionSet<FilterRenderingOption> renderingOptions, const GraphicsContext& destinationContext)
{
    bool hasFilterThatMovesPixels = filter.hasFilterThatMovesPixels();
    bool hasFilterThatShouldBeRestrictedBySecurityOrigin = filter.hasFilterThatShouldBeRestrictedBySecurityOrigin();

    Ref filterRenderer = adoptRef(*new CSSFilterRenderer(geometry, renderingOptions, hasFilterThatMovesPixels, hasFilterThatShouldBeRestrictedBySecurityOrigin));

    if (!filterRenderer->buildFilterFunctions(renderer, filter, preferredRenderingModes, renderingOptions, destinationContext)) {
        LOG_WITH_STREAM(Filters, stream << "CSSFilterRenderer::create: failed to build filters " << filter);
        return nullptr;
    }

    filterRenderer->setFilterRenderingModes(preferredRenderingModes);

    LOG_WITH_STREAM(Filters, stream << "CSSFilterRenderer::create built filter " << filterRenderer.get() << " for " << filter << " supported rendering mode(s) " << filterRenderer->filterRenderingModes());

    return filterRenderer;
}

Ref<CSSFilterRenderer> CSSFilterRenderer::create(Vector<Ref<FilterFunction>>&& functions, const FilterGeometry& geometry, OptionSet<FilterRenderingMode> preferredRenderingModes, OptionSet<FilterRenderingOption> renderingOptions)
{
    Ref filter = adoptRef(*new CSSFilterRenderer(WTF::move(functions), geometry, renderingOptions));
    // Setting filter rendering modes cannot be moved to the constructor because it ends up
    // calling supportedFilterRenderingModes() which is a virtual function.
    filter->setFilterRenderingModes(preferredRenderingModes);
    return filter;
}

CSSFilterRenderer::CSSFilterRenderer(const FilterGeometry& geometry, OptionSet<FilterRenderingOption> renderingOptions, bool hasFilterThatMovesPixels, bool hasFilterThatShouldBeRestrictedBySecurityOrigin)
    : Filter(Filter::Type::CSSFilterRenderer, geometry, renderingOptions)
    , m_hasFilterThatMovesPixels(hasFilterThatMovesPixels)
    , m_hasFilterThatShouldBeRestrictedBySecurityOrigin(hasFilterThatShouldBeRestrictedBySecurityOrigin)
{
}

CSSFilterRenderer::CSSFilterRenderer(Vector<Ref<FilterFunction>>&& functions, const FilterGeometry& geometry, OptionSet<FilterRenderingOption> renderingOptions)
    : Filter(Type::CSSFilterRenderer, geometry, renderingOptions)
    , m_functions(WTF::move(functions))
{
    clampFilterRegionIfNeeded();
}

static RefPtr<SVGFilterElement> referenceFilterElement(const Style::FilterReference& filterReference, const RenderElement& renderer)
{
    RefPtr filterElement = ReferencedSVGResources::referencedFilterElement(protect(renderer.treeScopeForSVGReferences()), filterReference);

    if (!filterElement) {
        LOG_WITH_STREAM(Filters, stream << " buildReferenceFilter: failed to find filter renderer, adding pending resource " << filterReference.url);
        // Although we did not find the referenced filter, it might exist later in the document.
        // FIXME: This skips anonymous RenderObjects. <https://webkit.org/b/131085>
        // FIXME: Unclear if this does anything.
        return nullptr;
    }

    return filterElement;
}

static bool isIdentityReferenceFilter(const Style::FilterReference& filterReference, const RenderElement& renderer)
{
    RefPtr filterElement = referenceFilterElement(filterReference, renderer);
    if (!filterElement)
        return false;

    return SVGFilterRenderer::isIdentity(*filterElement);
}

static IntOutsets calculateReferenceFilterOutsets(const Style::FilterReference& filterReference, const RenderElement& renderer, const FloatRect& targetBoundingBox)
{
    RefPtr filterElement = referenceFilterElement(filterReference, renderer);
    if (!filterElement)
        return { };

    return SVGFilterRenderer::calculateOutsets(*filterElement, targetBoundingBox);
}

static bool linearTransferCoefficients(const ComponentTransferFunction& function, float& slope, float& intercept)
{
    // A color matrix can only represent identity and linear transfer functions (out = slope * in + intercept).
    switch (function.type) {
    case ComponentTransferType::FECOMPONENTTRANSFER_TYPE_UNKNOWN:
    case ComponentTransferType::FECOMPONENTTRANSFER_TYPE_IDENTITY:
        slope = 1;
        intercept = 0;
        return true;
    case ComponentTransferType::FECOMPONENTTRANSFER_TYPE_LINEAR:
        slope = function.slope;
        intercept = function.intercept;
        return true;
    case ComponentTransferType::FECOMPONENTTRANSFER_TYPE_TABLE:
    case ComponentTransferType::FECOMPONENTTRANSFER_TYPE_DISCRETE:
    case ComponentTransferType::FECOMPONENTTRANSFER_TYPE_GAMMA:
        return false;
    }
    return false;
}

static std::optional<std::array<float, 20>> colorMatrixForReferenceFilter(const Style::FilterReference& filterReference, const RenderElement& renderer)
{
    RefPtr filterElement = referenceFilterElement(filterReference, renderer);
    if (!filterElement)
        return std::nullopt;

    // The compositor applies a color matrix to the whole layer and does not clip to a filter region,
    // so only the default region (which fully contains the element) can be matched exactly.
    if (filterElement->filterUnits() != SVGUnitTypes::SVG_UNIT_TYPE_OBJECTBOUNDINGBOX)
        return std::nullopt;
    if (filterElement->hasAttributeWithoutSynchronization(SVGNames::xAttr)
        || filterElement->hasAttributeWithoutSynchronization(SVGNames::yAttr)
        || filterElement->hasAttributeWithoutSynchronization(SVGNames::widthAttr)
        || filterElement->hasAttributeWithoutSynchronization(SVGNames::heightAttr))
        return std::nullopt;

    // The filter must be a single feComponentTransfer primitive.
    RefPtr<SVGFEComponentTransferElement> componentTransfer;
    for (Ref primitive : childrenOfType<SVGFilterPrimitiveStandardAttributes>(*filterElement)) {
        if (componentTransfer)
            return std::nullopt;
        componentTransfer = dynamicDowncast<SVGFEComponentTransferElement>(primitive.get());
        if (!componentTransfer)
            return std::nullopt;
    }
    if (!componentTransfer)
        return std::nullopt;

    // It must consume SourceGraphic (the default input) over the whole element (no primitive subregion).
    if (!componentTransfer->in1().isEmpty() || componentTransfer->effectGeometryFlags())
        return std::nullopt;

    ComponentTransferFunctions functions;
    for (Ref function : childrenOfType<SVGComponentTransferFunctionElement>(*componentTransfer))
        functions[function->channel()] = function->transferFunction();

    float slopeR, interceptR, slopeG, interceptG, slopeB, interceptB, slopeA, interceptA;
    if (!linearTransferCoefficients(functions[ComponentTransferChannel::Red], slopeR, interceptR)
        || !linearTransferCoefficients(functions[ComponentTransferChannel::Green], slopeG, interceptG)
        || !linearTransferCoefficients(functions[ComponentTransferChannel::Blue], slopeB, interceptB)
        || !linearTransferCoefficients(functions[ComponentTransferChannel::Alpha], slopeA, interceptA))
        return std::nullopt;

    // feComponentTransfer defaults to operating in linearRGB, but the compositor applies the matrix in
    // the layer's (sRGB/device) color space. Color space only affects the color channels, never alpha,
    // so the linearRGB case is only safe to composite when the color channels are left unchanged (the
    // common alpha-only "make edges opaque" workaround). See https://bugs.webkit.org/show_bug.cgi?id=287982.
    bool colorChannelsAreIdentity = slopeR == 1 && !interceptR && slopeG == 1 && !interceptG && slopeB == 1 && !interceptB;
    if (componentTransfer->colorInterpolation() == ColorInterpolation::LinearRGB && !colorChannelsAreIdentity)
        return std::nullopt;

    // Row-major { R, G, B, A } rows, each { r, g, b, a, bias }.
    return std::array<float, 20> { {
        slopeR, 0,      0,      0,      interceptR,
        0,      slopeG, 0,      0,      interceptG,
        0,      0,      slopeB, 0,      interceptB,
        0,      0,      0,      slopeA, interceptA,
    } };
}

std::optional<FilterOperations> CSSFilterRenderer::compositedColorMatrixFiltersForReferenceFilter(const RenderElement& renderer, const Style::Filter& filter)
{
    if (filter.isNone())
        return std::nullopt;

    Vector<Ref<FilterOperation>> operations;
    bool representable = true;
    for (auto& filterValue : filter) {
        WTF::switchOn(filterValue,
            [&](const Style::FilterReference& filterReference) {
                if (auto values = colorMatrixForReferenceFilter(filterReference, renderer))
                    operations.append(ColorMatrixFilterOperation::create(*values));
                else
                    representable = false;
            },
            [&](const auto&) {
                // Only pure reference filters are handled; mixing with CSS <filter-function>s
                // falls back to the software filter path.
                representable = false;
            }
        );
        if (!representable)
            return std::nullopt;
    }

    if (operations.isEmpty())
        return std::nullopt;

    return FilterOperations { WTF::move(operations) };
}

static RefPtr<SVGFilterRenderer> createReferenceFilter(const CSSFilterRenderer& filter, const Style::FilterReference& filterReference, RenderElement& renderer, OptionSet<FilterRenderingMode> preferredRenderingModes, OptionSet<FilterRenderingOption> renderingOptions, const GraphicsContext& destinationContext)
{
    RefPtr filterElement = referenceFilterElement(filterReference, renderer);
    if (!filterElement)
        return nullptr;

    RefPtr contextElement = dynamicDowncast<SVGElement>(renderer.element());

    auto geometry = filter.geometry();
    geometry.filterRegion = SVGLengthContext::resolveRectangle(contextElement.get(), *filterElement, filterElement->filterUnits(), filter.referenceBox());
    if (geometry.filterRegion.isEmpty())
        return nullptr;

    return SVGFilterRenderer::create(contextElement.get(), *filterElement, geometry, preferredRenderingModes, renderingOptions, destinationContext);
}

RefPtr<FilterFunction> CSSFilterRenderer::buildFilterFunction(RenderElement& renderer, const Style::FilterValue& filterValue, OptionSet<FilterRenderingMode> preferredRenderingModes, OptionSet<FilterRenderingOption> renderingOptions, const GraphicsContext& destinationContext)
{
    return WTF::switchOn(filterValue,
        [&](const Style::FilterReference& filterReference) -> RefPtr<FilterFunction> {
            return createReferenceFilter(*this, filterReference, renderer, preferredRenderingModes, renderingOptions, destinationContext);
        },
        [&](const Style::BlurFunction& blurFunction) -> RefPtr<FilterFunction> {
            return Style::evaluate<Ref<FilterEffect>>(blurFunction, renderer.style());
        },
        [&](const Style::DropShadowFunction& dropShadowFunction) -> RefPtr<FilterFunction> {
            return Style::evaluate<Ref<FilterEffect>>(dropShadowFunction, renderer.style());
        },
        []<CSSValueID C, typename T>(const FunctionNotation<C, T>& filterFunction) -> RefPtr<FilterFunction> {
            return Style::evaluate<Ref<FilterEffect>>(filterFunction);
        }
    );
}

bool CSSFilterRenderer::buildFilterFunctions(RenderElement& renderer, const Style::Filter& filter, OptionSet<FilterRenderingMode> preferredRenderingModes, OptionSet<FilterRenderingOption> renderingOptions, const GraphicsContext& destinationContext)
{
    for (auto& value : filter) {
        auto function = buildFilterFunction(renderer, value, preferredRenderingModes, renderingOptions, destinationContext);
        if (!function)
            continue;

        if (m_functions.isEmpty())
            m_functions.append(SourceGraphic::create());

        m_functions.append(function.releaseNonNull());
    }

    // If we didn't make any effects, tell our caller we are not valid.
    if (m_functions.isEmpty())
        return false;

    m_functions.shrinkToFit();
    return true;
}

FilterEffectVector CSSFilterRenderer::effectsOfType(FilterFunction::Type filterType) const
{
    FilterEffectVector effects;

    for (auto& function : m_functions) {
        if (function->filterType() == filterType) {
            effects.append({ downcast<FilterEffect>(function.get()) });
            continue;
        }

        if (RefPtr filter = dynamicDowncast<SVGFilterRenderer>(function))
            effects.appendVector(filter->effectsOfType(filterType));
    }

    return effects;
}

OptionSet<FilterRenderingMode> CSSFilterRenderer::supportedFilterRenderingModes(OptionSet<FilterRenderingMode> preferredFilterRenderingModes) const
{
    OptionSet<FilterRenderingMode> modes = allFilterRenderingModes;

    for (auto& function : m_functions)
        modes = modes & function->supportedFilterRenderingModes(preferredFilterRenderingModes);

    ASSERT(modes);
    return modes;
}

void CSSFilterRenderer::computeEnclosingFilterRegion()
{
#if USE(CORE_IMAGE)
    auto enclosingFilterRegion = filterRegion();
    for (auto& function : m_functions) {
        if (RefPtr filter = dynamicDowncast<Filter>(function))
            enclosingFilterRegion.unite(filter->filterRegion());
    }
    setEnclosingFilterRegion(enclosingFilterRegion);
#endif
}

RefPtr<FilterImage> CSSFilterRenderer::apply(FilterImage* sourceImage, FilterResults& results)
{
    ASSERT(filterRenderingModes().contains(FilterRenderingMode::Software));

    if (!sourceImage)
        return nullptr;

    LOG_WITH_STREAM(Filters, stream << "\nCSSFilterRenderer " << this << " apply - filterRegion " << filterRegion() << " scale " << filterScale());
    RefPtr<FilterImage> result = sourceImage;

    for (auto& function : m_functions) {
        result = function->apply(*this, *result, results);
        if (!result)
            return nullptr;
    }

    return result;
}

FilterStyleVector CSSFilterRenderer::createFilterStyles(GraphicsContext& context, const FilterStyle& sourceStyle) const
{
    ASSERT(filterRenderingModes().contains(FilterRenderingMode::GraphicsContext));

    FilterStyleVector styles;
    FilterStyle lastStyle = sourceStyle;

    for (auto& function : m_functions) {
        if (function->filterType() == FilterEffect::Type::SourceGraphic)
            continue;

        auto result = function->createFilterStyles(context, *this, lastStyle);
        if (result.isEmpty())
            return { };

        lastStyle = result.last();
        styles.appendVector(WTF::move(result));
    }

    return styles;
}

void CSSFilterRenderer::setFilterRegion(const FloatRect& filterRegion)
{
    Filter::setFilterRegion(filterRegion);
    clampFilterRegionIfNeeded();
}

bool CSSFilterRenderer::isIdentity(const RenderElement& renderer, const Style::Filter& filter)
{
    if (filter.hasFilterThatShouldBeRestrictedBySecurityOrigin())
        return false;

    for (auto& value : filter) {
        auto hasNonIdentityComponent = WTF::switchOn(value,
            [&](const Style::FilterReference& filterReference) {
                return !isIdentityReferenceFilter(filterReference, renderer);
            },
            [](const auto& filterFunction) {
                return !filterFunction->isIdentity();
            }
        );
        if (hasNonIdentityComponent)
            return false;
    }

    return true;
}

IntOutsets CSSFilterRenderer::calculateOutsets(const RenderElement& renderer, const Style::Filter& filter, const FloatRect& targetBoundingBox)
{
    IntOutsets outsets;

    auto zoom = renderer.style().usedZoomForLength();

    for (auto& value : filter) {
        WTF::switchOn(value,
            [&](const Style::FilterReference& filterReference) {
                outsets += calculateReferenceFilterOutsets(filterReference, renderer, targetBoundingBox);
            },
            [&](const Style::BlurFunction& blurFunction) {
                outsets += blurFunction->calculateOutsets(zoom);
            },
            [&](const Style::DropShadowFunction& dropShadowFunction) {
                outsets += dropShadowFunction->calculateOutsets(zoom);
            },
            []<CSSValueID C, typename T>(const FunctionNotation<C, T>&) { }
        );
    }

    return outsets;
}

TextStream& CSSFilterRenderer::externalRepresentation(TextStream& ts, FilterRepresentation representation) const
{
    unsigned level = 0;

    for (auto it = m_functions.rbegin(), end = m_functions.rend(); it != end; ++it) {
        auto& function = *it;

        // SourceAlpha is a built-in effect. No need to say SourceGraphic is its input.
        if (function->filterType() == FilterEffect::Type::SourceAlpha)
            ++it;

        TextStream::IndentScope indentScope(ts, level++);
        function->externalRepresentation(ts, representation);
    }

    return ts;
}

} // namespace WebCore

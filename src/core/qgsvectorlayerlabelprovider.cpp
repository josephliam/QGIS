/***************************************************************************
  qgsvectorlayerlabelprovider.cpp
  --------------------------------------
  Date                 : September 2015
  Copyright            : (C) 2015 by Martin Dobias
  Email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsvectorlayerlabelprovider.h"

#include "qgsdatadefined.h"
#include "qgsgeometry.h"
#include "qgslabelsearchtree.h"
#include "qgspallabeling.h"
#include "qgstextlabelfeature.h"
#include "qgsvectorlayer.h"
#include "qgsvectorlayerfeatureiterator.h"
#include "qgsrenderer.h"
#include "qgspolygon.h"
#include "qgslinestring.h"
#include "qgsmultipolygon.h"
#include "qgslogger.h"

#include "feature.h"
#include "labelposition.h"

#include <QPicture>

using namespace pal;

QgsVectorLayerLabelProvider::QgsVectorLayerLabelProvider( QgsVectorLayer* layer, const QString& providerId, bool withFeatureLoop, const QgsPalLayerSettings* settings, const QString& layerName )
    : QgsAbstractLabelProvider( layer->id(), providerId )
    , mSettings( settings ? *settings : QgsPalLayerSettings::fromLayer( layer ) )
    , mLayerGeometryType( layer->geometryType() )
    , mRenderer( layer->renderer() )
    , mFields( layer->fields() )
    , mCrs( layer->crs() )
{
  mName = layerName.isEmpty() ? layer->id() : layerName;

  if ( withFeatureLoop )
  {
    mSource = new QgsVectorLayerFeatureSource( layer );
    mOwnsSource = true;
  }
  else
  {
    mSource = nullptr;
    mOwnsSource = false;
  }

  init();
}

QgsVectorLayerLabelProvider::QgsVectorLayerLabelProvider( const QgsPalLayerSettings& settings,
    const QString& layerId,
    const QgsFields& fields,
    const QgsCoordinateReferenceSystem& crs,
    QgsAbstractFeatureSource* source,
    bool ownsSource, QgsFeatureRenderer* renderer )
    : QgsAbstractLabelProvider( layerId )
    , mSettings( settings )
    , mLayerGeometryType( QgsWkbTypes::UnknownGeometry )
    , mRenderer( renderer )
    , mFields( fields )
    , mCrs( crs )
    , mSource( source )
    , mOwnsSource( ownsSource )
{
  init();
}


void QgsVectorLayerLabelProvider::init()
{
  mPlacement = mSettings.placement;
  mLinePlacementFlags = mSettings.placementFlags;
  mFlags = Flags();
  if ( mSettings.drawLabels ) mFlags |= DrawLabels;
  if ( mSettings.displayAll ) mFlags |= DrawAllLabels;
  if ( mSettings.mergeLines ) mFlags |= MergeConnectedLines;
  if ( mSettings.centroidInside ) mFlags |= CentroidMustBeInside;
  if ( mSettings.labelPerPart ) mFlags |= LabelPerFeaturePart;
  mPriority = 1 - mSettings.priority / 10.0; // convert 0..10 --> 1..0

  if ( mLayerGeometryType == QgsWkbTypes::PointGeometry && mRenderer )
  {
    //override obstacle type to treat any intersection of a label with the point symbol as a high cost conflict
    mObstacleType = QgsPalLayerSettings::PolygonWhole;
  }
  else
  {
    mObstacleType = mSettings.obstacleType;
  }

  mUpsidedownLabels = mSettings.upsidedownLabels;
}


QgsVectorLayerLabelProvider::~QgsVectorLayerLabelProvider()
{
  qDeleteAll( mLabels );

  if ( mOwnsSource )
    delete mSource;
}


bool QgsVectorLayerLabelProvider::prepare( const QgsRenderContext& context, QSet<QString>& attributeNames )
{
  QgsPalLayerSettings& lyr = mSettings;
  const QgsMapSettings& mapSettings = mEngine->mapSettings();

  QgsDebugMsgLevel( "PREPARE LAYER " + mLayerId, 4 );

  if ( lyr.drawLabels )
  {
    if ( lyr.fieldName.isEmpty() )
    {
      return false;
    }

    if ( lyr.isExpression )
    {
      QgsExpression exp( lyr.fieldName );
      if ( exp.hasEvalError() )
      {
        QgsDebugMsgLevel( "Prepare error:" + exp.evalErrorString(), 4 );
        return false;
      }
    }
    else
    {
      // If we aren't an expression, we check to see if we can find the column.
      if ( mFields.lookupField( lyr.fieldName ) == -1 )
      {
        return false;
      }
    }
  }

  lyr.mCurFields = mFields;

  if ( lyr.drawLabels )
  {
    // add field indices for label's text, from expression or field
    if ( lyr.isExpression )
    {
      // prepare expression for use in QgsPalLayerSettings::registerFeature()
      QgsExpression* exp = lyr.getLabelExpression();
      exp->prepare( &context.expressionContext() );
      if ( exp->hasEvalError() )
      {
        QgsDebugMsgLevel( "Prepare error:" + exp->evalErrorString(), 4 );
      }
      Q_FOREACH ( const QString& name, exp->referencedColumns() )
      {
        QgsDebugMsgLevel( "REFERENCED COLUMN = " + name, 4 );
        attributeNames.insert( name );
      }
    }
    else
    {
      attributeNames.insert( lyr.fieldName );
    }

    // add field indices of data defined expression or field
    QMap< QgsPalLayerSettings::DataDefinedProperties, QgsDataDefined* >::const_iterator dIt = lyr.dataDefinedProperties.constBegin();
    for ( ; dIt != lyr.dataDefinedProperties.constEnd(); ++dIt )
    {
      QgsDataDefined* dd = dIt.value();
      if ( !dd->isActive() )
      {
        continue;
      }

      // this will return columns for expressions or field name, depending upon what is set to be used
      // this also prepares any expressions, too
      QSet<QString> cols = dd->referencedColumns( context.expressionContext() );

      //QgsDebugMsgLevel( QString( "Data defined referenced columns:" ) + cols.join( "," ), 4 );
      Q_FOREACH ( const QString& name, cols )
      {
        attributeNames.insert( name );
      }
    }
  }

  // NOW INITIALIZE QgsPalLayerSettings

  // TODO: ideally these (non-configuration) members should get out of QgsPalLayerSettings to here
  // (together with registerFeature() & related methods) and QgsPalLayerSettings just stores config

  // save the pal layer to our layer context (with some additional info)
  lyr.fieldIndex = mFields.lookupField( lyr.fieldName );

  lyr.xform = &mapSettings.mapToPixel();
  lyr.ct = QgsCoordinateTransform();
  if ( mapSettings.hasCrsTransformEnabled() )
  {
    if ( context.coordinateTransform().isValid() )
      // this is context for layer rendering - use its CT as it includes correct datum transform
      lyr.ct = context.coordinateTransform();
    else
      // otherwise fall back to creating our own CT - this one may not have the correct datum transform!
      lyr.ct = QgsCoordinateTransform( mCrs, mapSettings.destinationCrs() );
  }
  lyr.ptZero = lyr.xform->toMapCoordinates( 0, 0 );
  lyr.ptOne = lyr.xform->toMapCoordinates( 1, 0 );

  // rect for clipping
  lyr.extentGeom = QgsGeometry::fromRect( mapSettings.visibleExtent() );
  if ( !qgsDoubleNear( mapSettings.rotation(), 0.0 ) )
  {
    //PAL features are prerotated, so extent also needs to be unrotated
    lyr.extentGeom.rotate( -mapSettings.rotation(), mapSettings.visibleExtent().center() );
  }

  lyr.mFeatsSendingToPal = 0;

  return true;
}



QList<QgsLabelFeature*> QgsVectorLayerLabelProvider::labelFeatures( QgsRenderContext &ctx )
{
  if ( !mSource )
  {
    // we have created the provider with "own feature loop" == false
    // so it is assumed that prepare() has been already called followed by registerFeature() calls
    return mLabels;
  }

  QSet<QString> attrNames;
  if ( !prepare( ctx, attrNames ) )
    return QList<QgsLabelFeature*>();

  if ( mRenderer )
    mRenderer->startRender( ctx, mFields );

  QgsRectangle layerExtent = ctx.extent();
  if ( mSettings.ct.isValid() && !mSettings.ct.isShortCircuited() )
    layerExtent = mSettings.ct.transformBoundingBox( ctx.extent(), QgsCoordinateTransform::ReverseTransform );

  QgsFeatureRequest request;
  request.setFilterRect( layerExtent );
  request.setSubsetOfAttributes( attrNames, mFields );
  QgsFeatureIterator fit = mSource->getFeatures( request );

  QgsExpressionContextScope* symbolScope = new QgsExpressionContextScope();
  ctx.expressionContext().appendScope( symbolScope );
  QgsFeature fet;
  while ( fit.nextFeature( fet ) )
  {
    QScopedPointer<QgsGeometry> obstacleGeometry;
    if ( mRenderer )
    {
      QgsSymbolList symbols = mRenderer->originalSymbolsForFeature( fet, ctx );
      if ( !symbols.isEmpty() && fet.geometry().type() == QgsWkbTypes::PointGeometry )
      {
        //point feature, use symbol bounds as obstacle
        obstacleGeometry.reset( QgsVectorLayerLabelProvider::getPointObstacleGeometry( fet, ctx, symbols ) );
      }
      if ( !symbols.isEmpty() )
      {
        symbolScope = QgsExpressionContextUtils::updateSymbolScope( symbols.at( 0 ), symbolScope );
      }
    }
    ctx.expressionContext().setFeature( fet );
    registerFeature( fet, ctx, obstacleGeometry.data() );
  }

  if ( ctx.expressionContext().lastScope() == symbolScope )
    delete ctx.expressionContext().popScope();

  if ( mRenderer )
    mRenderer->stopRender( ctx );

  return mLabels;
}

void QgsVectorLayerLabelProvider::registerFeature( QgsFeature& feature, QgsRenderContext& context, QgsGeometry* obstacleGeometry )
{
  QgsLabelFeature* label = nullptr;
  mSettings.registerFeature( feature, context, &label, obstacleGeometry );
  if ( label )
    mLabels << label;
}

QgsGeometry* QgsVectorLayerLabelProvider::getPointObstacleGeometry( QgsFeature& fet, QgsRenderContext& context, const QgsSymbolList& symbols )
{
  if ( !fet.hasGeometry() || fet.geometry().type() != QgsWkbTypes::PointGeometry )
    return nullptr;

  bool isMultiPoint = fet.geometry().geometry()->nCoordinates() > 1;
  QgsAbstractGeometry* obstacleGeom = nullptr;
  if ( isMultiPoint )
    obstacleGeom = new QgsMultiPolygonV2();

  // for each point
  for ( int i = 0; i < fet.geometry().geometry()->nCoordinates(); ++i )
  {
    QRectF bounds;
    QgsPointV2 p =  fet.geometry().geometry()->vertexAt( QgsVertexId( i, 0, 0 ) );
    double x = p.x();
    double y = p.y();
    double z = 0; // dummy variable for coordinate transforms

    //transform point to pixels
    if ( context.coordinateTransform().isValid() )
    {
      context.coordinateTransform().transformInPlace( x, y, z );
    }
    context.mapToPixel().transformInPlace( x, y );

    QPointF pt( x, y );
    Q_FOREACH ( QgsSymbol* symbol, symbols )
    {
      if ( symbol->type() == QgsSymbol::Marker )
      {
        if ( bounds.isValid() )
          bounds = bounds.united( static_cast< QgsMarkerSymbol* >( symbol )->bounds( pt, context, fet ) );
        else
          bounds = static_cast< QgsMarkerSymbol* >( symbol )->bounds( pt, context, fet );
      }
    }

    //convert bounds to a geometry
    QgsLineString* boundLineString = new QgsLineString();
    boundLineString->addVertex( QgsPointV2( bounds.topLeft() ) );
    boundLineString->addVertex( QgsPointV2( bounds.topRight() ) );
    boundLineString->addVertex( QgsPointV2( bounds.bottomRight() ) );
    boundLineString->addVertex( QgsPointV2( bounds.bottomLeft() ) );

    //then transform back to map units
    //TODO - remove when labeling is refactored to use screen units
    for ( int i = 0; i < boundLineString->numPoints(); ++i )
    {
      QgsPoint point = context.mapToPixel().toMapCoordinates( boundLineString->xAt( i ), boundLineString->yAt( i ) );
      boundLineString->setXAt( i, point.x() );
      boundLineString->setYAt( i, point.y() );
    }
    if ( context.coordinateTransform().isValid() )
    {
      boundLineString->transform( context.coordinateTransform(), QgsCoordinateTransform::ReverseTransform );
    }
    boundLineString->close();

    QgsPolygonV2* obstaclePolygon = new QgsPolygonV2();
    obstaclePolygon->setExteriorRing( boundLineString );

    if ( isMultiPoint )
    {
      static_cast<QgsMultiPolygonV2*>( obstacleGeom )->addGeometry( obstaclePolygon );
    }
    else
    {
      obstacleGeom = obstaclePolygon;
    }
  }

  return new QgsGeometry( obstacleGeom );
}

void QgsVectorLayerLabelProvider::drawLabel( QgsRenderContext& context, pal::LabelPosition* label ) const
{
  if ( !mSettings.drawLabels )
    return;

  QgsTextLabelFeature* lf = dynamic_cast<QgsTextLabelFeature*>( label->getFeaturePart()->feature() );

  // Copy to temp, editable layer settings
  // these settings will be changed by any data defined values, then used for rendering label components
  // settings may be adjusted during rendering of components
  QgsPalLayerSettings tmpLyr( mSettings );

  // apply any previously applied data defined settings for the label
  const QMap< QgsPalLayerSettings::DataDefinedProperties, QVariant >& ddValues = lf->dataDefinedValues();

  //font
  QFont dFont = lf->definedFont();
  QgsDebugMsgLevel( QString( "PAL font tmpLyr: %1, Style: %2" ).arg( tmpLyr.format().font().toString(), tmpLyr.format().font().styleName() ), 4 );
  QgsDebugMsgLevel( QString( "PAL font definedFont: %1, Style: %2" ).arg( dFont.toString(), dFont.styleName() ), 4 );

  QgsTextFormat format = tmpLyr.format();
  format.setFont( dFont );

  // size has already been calculated and stored in the defined font - this calculated size
  // is in pixels
  format.setSize( dFont.pixelSize() );
  format.setSizeUnit( QgsUnitTypes::RenderPixels );
  tmpLyr.setFormat( format );

  if ( tmpLyr.multilineAlign == QgsPalLayerSettings::MultiFollowPlacement )
  {
    //calculate font alignment based on label quadrant
    switch ( label->getQuadrant() )
    {
      case LabelPosition::QuadrantAboveLeft:
      case LabelPosition::QuadrantLeft:
      case LabelPosition::QuadrantBelowLeft:
        tmpLyr.multilineAlign = QgsPalLayerSettings::MultiRight;
        break;
      case LabelPosition::QuadrantAbove:
      case LabelPosition::QuadrantOver:
      case LabelPosition::QuadrantBelow:
        tmpLyr.multilineAlign = QgsPalLayerSettings::MultiCenter;
        break;
      case LabelPosition::QuadrantAboveRight:
      case LabelPosition::QuadrantRight:
      case LabelPosition::QuadrantBelowRight:
        tmpLyr.multilineAlign = QgsPalLayerSettings::MultiLeft;
        break;
    }
  }

  // update tmpLyr with any data defined text style values
  QgsPalLabeling::dataDefinedTextStyle( tmpLyr, ddValues );

  // update tmpLyr with any data defined text buffer values
  QgsPalLabeling::dataDefinedTextBuffer( tmpLyr, ddValues );

  // update tmpLyr with any data defined text formatting values
  QgsPalLabeling::dataDefinedTextFormatting( tmpLyr, ddValues );

  // update tmpLyr with any data defined shape background values
  QgsPalLabeling::dataDefinedShapeBackground( tmpLyr, ddValues );

  // update tmpLyr with any data defined drop shadow values
  QgsPalLabeling::dataDefinedDropShadow( tmpLyr, ddValues );

  // Render the components of a label in reverse order
  //   (backgrounds -> text)

  if ( tmpLyr.format().shadow().enabled() && tmpLyr.format().shadow().shadowPlacement() == QgsTextShadowSettings::ShadowLowest )
  {
    QgsTextFormat format = tmpLyr.format();

    if ( tmpLyr.format().background().enabled() )
    {
      format.shadow().setShadowPlacement( QgsTextShadowSettings::ShadowShape );
    }
    else if ( tmpLyr.format().buffer().enabled() )
    {
      format.shadow().setShadowPlacement( QgsTextShadowSettings::ShadowBuffer );
    }
    else
    {
      format.shadow().setShadowPlacement( QgsTextShadowSettings::ShadowText );
    }

    tmpLyr.setFormat( format );
  }

  if ( tmpLyr.format().background().enabled() )
  {
    drawLabelPrivate( label, context, tmpLyr, QgsTextRenderer::Background );
  }

  if ( tmpLyr.format().buffer().enabled() )
  {
    drawLabelPrivate( label, context, tmpLyr, QgsTextRenderer::Buffer );
  }

  drawLabelPrivate( label, context, tmpLyr, QgsTextRenderer::Text );

  // add to the results
  QString labeltext = label->getFeaturePart()->feature()->labelText();
  mEngine->results()->mLabelSearchTree->insertLabel( label, label->getFeaturePart()->featureId(), mLayerId, labeltext, dFont, false, lf->hasFixedPosition(), mProviderId );
}


void QgsVectorLayerLabelProvider::drawLabelPrivate( pal::LabelPosition* label, QgsRenderContext& context, QgsPalLayerSettings& tmpLyr, QgsTextRenderer::TextPart drawType, double dpiRatio ) const
{
  // NOTE: this is repeatedly called for multi-part labels
  QPainter* painter = context.painter();

  // features are pre-rotated but not scaled/translated,
  // so we only disable rotation here. Ideally, they'd be
  // also pre-scaled/translated, as suggested here:
  // http://hub.qgis.org/issues/11856
  QgsMapToPixel xform = context.mapToPixel();
  xform.setMapRotation( 0, 0, 0 );

  QPointF outPt = xform.transform( label->getX(), label->getY() ).toQPointF();

  if ( mEngine->testFlag( QgsLabelingEngine::DrawLabelRectOnly ) )  // TODO: this should get directly to labeling engine
  {
    //debugging rect
    if ( drawType != QgsTextRenderer::Text )
      return;

    QgsPoint outPt2 = xform.transform( label->getX() + label->getWidth(), label->getY() + label->getHeight() );
    QRectF rect( 0, 0, outPt2.x() - outPt.x(), outPt2.y() - outPt.y() );
    painter->save();
    painter->setRenderHint( QPainter::Antialiasing, false );
    painter->translate( QPointF( outPt.x(), outPt.y() ) );
    painter->rotate( -label->getAlpha() * 180 / M_PI );

    if ( label->conflictsWithObstacle() )
    {
      painter->setBrush( QColor( 255, 0, 0, 100 ) );
      painter->setPen( QColor( 255, 0, 0, 150 ) );
    }
    else
    {
      painter->setBrush( QColor( 0, 255, 0, 100 ) );
      painter->setPen( QColor( 0, 255, 0, 150 ) );
    }

    painter->drawRect( rect );
    painter->restore();

    if ( label->getNextPart() )
      drawLabelPrivate( label->getNextPart(), context, tmpLyr, drawType, dpiRatio );

    return;
  }

  QgsTextRenderer::Component component;
  component.dpiRatio = dpiRatio;
  component.origin = outPt;
  component.rotation = label->getAlpha();



  if ( drawType == QgsTextRenderer::Background )
  {
    // get rotated label's center point
    QPointF centerPt( outPt );
    QgsPoint outPt2 = xform.transform( label->getX() + label->getWidth() / 2,
                                       label->getY() + label->getHeight() / 2 );

    double xc = outPt2.x() - outPt.x();
    double yc = outPt2.y() - outPt.y();

    double angle = -component.rotation;
    double xd = xc * cos( angle ) - yc * sin( angle );
    double yd = xc * sin( angle ) + yc * cos( angle );

    centerPt.setX( centerPt.x() + xd );
    centerPt.setY( centerPt.y() + yd );

    component.center = centerPt;

    // convert label size to render units
    double labelWidthPx = QgsTextRenderer::scaleToPixelContext( label->getWidth(), context, QgsUnitTypes::RenderMapUnits, true );
    double labelHeightPx = QgsTextRenderer::scaleToPixelContext( label->getHeight(), context, QgsUnitTypes::RenderMapUnits, true );

    component.size = QSizeF( labelWidthPx, labelHeightPx );

    QgsTextRenderer::drawBackground( context, component, tmpLyr.format(), QStringList(), QgsTextRenderer::Label );
  }

  else if ( drawType == QgsTextRenderer::Buffer
            || drawType == QgsTextRenderer::Text )
  {

    // TODO: optimize access :)
    QgsTextLabelFeature* lf = static_cast<QgsTextLabelFeature*>( label->getFeaturePart()->feature() );
    QString txt = lf->text( label->getPartId() );
    QFontMetricsF* labelfm = lf->labelFontMetrics();

    //add the direction symbol if needed
    if ( !txt.isEmpty() && tmpLyr.placement == QgsPalLayerSettings::Line &&
         tmpLyr.addDirectionSymbol )
    {
      bool prependSymb = false;
      QString symb = tmpLyr.rightDirectionSymbol;

      if ( label->getReversed() )
      {
        prependSymb = true;
        symb = tmpLyr.leftDirectionSymbol;
      }

      if ( tmpLyr.reverseDirectionSymbol )
      {
        if ( symb == tmpLyr.rightDirectionSymbol )
        {
          prependSymb = true;
          symb = tmpLyr.leftDirectionSymbol;
        }
        else
        {
          prependSymb = false;
          symb = tmpLyr.rightDirectionSymbol;
        }
      }

      if ( tmpLyr.placeDirectionSymbol == QgsPalLayerSettings::SymbolAbove )
      {
        prependSymb = true;
        symb = symb + QStringLiteral( "\n" );
      }
      else if ( tmpLyr.placeDirectionSymbol == QgsPalLayerSettings::SymbolBelow )
      {
        prependSymb = false;
        symb = QStringLiteral( "\n" ) + symb;
      }

      if ( prependSymb )
      {
        txt.prepend( symb );
      }
      else
      {
        txt.append( symb );
      }
    }

    //QgsDebugMsgLevel( "drawLabel " + txt, 4 );
    QStringList multiLineList = QgsPalLabeling::splitToLines( txt, tmpLyr.wrapChar );

    QgsTextRenderer::HAlignment hAlign = QgsTextRenderer::AlignLeft;
    if ( tmpLyr.multilineAlign == QgsPalLayerSettings::MultiCenter )
      hAlign = QgsTextRenderer::AlignCenter;
    else if ( tmpLyr.multilineAlign == QgsPalLayerSettings::MultiRight )
      hAlign = QgsTextRenderer::AlignRight;

    QgsTextRenderer::Component component;
    component.origin = outPt;
    component.rotation = label->getAlpha();
    QgsTextRenderer::drawTextInternal( drawType, context, tmpLyr.format(), component, multiLineList, labelfm,
                                       hAlign, mEngine->testFlag( QgsLabelingEngine::RenderOutlineLabels ), QgsTextRenderer::Label );

  }

  // NOTE: this used to be within above multi-line loop block, at end. (a mistake since 2010? [LS])
  if ( label->getNextPart() )
    drawLabelPrivate( label->getNextPart(), context, tmpLyr, drawType, dpiRatio );
}

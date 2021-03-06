/***************************************************************************
           qgsvirtuallayerprovider.cpp Virtual layer data provider
begin                : Jan, 2015
copyright            : (C) 2015 Hugo Mercier, Oslandia
email                : hugo dot mercier at oslandia dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSVIRTUAL_LAYER_PROVIDER_H
#define QGSVIRTUAL_LAYER_PROVIDER_H

#include <qgsvectordataprovider.h>

#include "qgscoordinatereferencesystem.h"
#include "qgsvirtuallayerdefinition.h"
#include "qgsvirtuallayersqlitehelper.h"

class QgsVirtualLayerFeatureIterator;

class QgsVirtualLayerProvider: public QgsVectorDataProvider
{
    Q_OBJECT
  public:

    /**
     * Constructor of the vector provider
     * @param uri  uniform resource locator (URI) for a dataset
     */
    explicit QgsVirtualLayerProvider( QString const &uri = QLatin1String( "" ) );

    /** Destructor */
    virtual ~QgsVirtualLayerProvider();

    virtual QgsAbstractFeatureSource* featureSource() const override;

    /** Returns the permanent storage type for this layer as a friendly name */
    virtual QString storageType() const override;

    virtual QgsCoordinateReferenceSystem crs() const override;

    virtual QgsFeatureIterator getFeatures( const QgsFeatureRequest& request ) const override;

    /** Get the feature geometry type */
    QgsWkbTypes::Type wkbType() const override;

    /** Get the number of features in the layer */
    long featureCount() const override;

    virtual QgsRectangle extent() const override;

    virtual QString subsetString() const override;

    /** Set the subset string used to create a subset of features in the layer (WHERE clause) */
    virtual bool setSubsetString( const QString& subset, bool updateFeatureCount = true ) override;

    virtual bool supportsSubsetString() const override { return true; }

    QgsFields fields() const override;

    bool isValid() const override;

    /** Returns a bitmask containing the supported capabilities*/
    QgsVectorDataProvider::Capabilities capabilities() const override;

    /** Return the provider name */
    QString name() const override;

    /** Return description  */
    QString description() const override;

    QgsAttributeList pkAttributeIndexes() const override;

    /** Get the list of layer ids on which this layer depends */
    QSet<QgsMapLayerDependency> dependencies() const override;

  private:

    // file on disk
    QString mPath;

    QgsScopedSqlite mSqlite;

    // underlying vector layers
    struct SourceLayer
    {
      SourceLayer(): layer( nullptr ) {}
      SourceLayer( QgsVectorLayer *l, const QString& n = QLatin1String( "" ) )
          : layer( l )
          , name( n )
      {}
      SourceLayer( const QString& p, const QString& s, const QString& n, const QString& e = QStringLiteral( "UTF-8" ) )
          : layer( nullptr )
          , name( n )
          , source( s )
          , provider( p )
          , encoding( e )
      {}
      // non-null if it refers to a live layer
      QgsVectorLayer* layer;
      QString name;
      // non-empty if it is an embedded layer
      QString source;
      QString provider;
      QString encoding;
    };
    typedef QVector<SourceLayer> SourceLayers;
    SourceLayers mLayers;


    bool mValid;

    QString mTableName;

    QgsCoordinateReferenceSystem mCrs;

    QgsVirtualLayerDefinition mDefinition;

    QString mSubset;

    void resetSqlite();

    mutable bool mCachedStatistics;
    mutable qint64 mFeatureCount;
    mutable QgsRectangle mExtent;

    void updateStatistics() const;

    bool openIt();
    bool createIt();
    bool loadSourceLayers();

    friend class QgsVirtualLayerFeatureIterator;

  private slots:
    void invalidateStatistics();
};

#endif

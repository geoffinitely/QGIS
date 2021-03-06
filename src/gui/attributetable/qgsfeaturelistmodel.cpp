/***************************************************************************
    qgsfeaturelistmodel.cpp
    ---------------------
    begin                : February 2013
    copyright            : (C) 2013 by Matthias Kuhn
    email                : matthias at opengis dot ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgsexception.h"
#include "qgsvectordataprovider.h"
#include "qgsfeaturelistmodel.h"
#include "qgsattributetablemodel.h"
#include "qgsvectorlayereditbuffer.h"
#include "qgsattributetablefiltermodel.h"

#include <QItemSelection>
#include <QSettings>

QgsFeatureListModel::QgsFeatureListModel( QgsAttributeTableFilterModel *sourceModel, QObject *parent )
  : QAbstractProxyModel( parent )
  , mInjectNull( false )
{
  setSourceModel( sourceModel );
}

QgsFeatureListModel::~QgsFeatureListModel()
{
}

void QgsFeatureListModel::setSourceModel( QgsAttributeTableFilterModel *sourceModel )
{
  QAbstractProxyModel::setSourceModel( sourceModel );
  mExpressionContext = sourceModel->layer()->createExpressionContext();
  mFilterModel = sourceModel;

  if ( mFilterModel )
  {
    // rewire (filter-)change events in the source model so this proxy reflects the changes
    connect( mFilterModel, &QAbstractItemModel::rowsAboutToBeRemoved, this, &QgsFeatureListModel::onBeginRemoveRows );
    connect( mFilterModel, &QAbstractItemModel::rowsRemoved, this, &QgsFeatureListModel::onEndRemoveRows );
    connect( mFilterModel, &QAbstractItemModel::rowsAboutToBeInserted, this, &QgsFeatureListModel::onBeginInsertRows );
    connect( mFilterModel, &QAbstractItemModel::rowsInserted, this, &QgsFeatureListModel::onEndInsertRows );
    // propagate sort order changes from source model to views connected to this model
    connect( mFilterModel, &QAbstractItemModel::layoutAboutToBeChanged, this, &QAbstractItemModel::layoutAboutToBeChanged );
    connect( mFilterModel, &QAbstractItemModel::layoutChanged, this, &QAbstractItemModel::layoutChanged );
  }
}

QgsVectorLayerCache *QgsFeatureListModel::layerCache()
{
  return mFilterModel->layerCache();
}

QgsFeatureId QgsFeatureListModel::idxToFid( const QModelIndex &index ) const
{
  return mFilterModel->masterModel()->rowToId( mapToMaster( index ).row() );
}

QModelIndex QgsFeatureListModel::fidToIdx( const QgsFeatureId fid ) const
{
  return mFilterModel->mapFromMaster( mFilterModel->masterModel()->idToIndex( fid ) );
}

QVariant QgsFeatureListModel::data( const QModelIndex &index, int role ) const
{
  if ( mInjectNull && index.row() == 0 )
  {
    if ( role == Qt::DisplayRole )
    {
      return QgsApplication::nullRepresentation();
    }
    else
    {
      return QVariant( QVariant::Invalid );
    }
  }

  if ( role == Qt::DisplayRole || role == Qt::EditRole )
  {
    QgsFeature feat;

    mFilterModel->layerCache()->featureAtId( idxToFid( index ), feat );

    mExpressionContext.setFeature( feat );
    return mDisplayExpression.evaluate( &mExpressionContext );
  }

  if ( role == FeatureInfoRole )
  {
    FeatureInfo featInfo;

    QgsFeature feat;

    mFilterModel->layerCache()->featureAtId( idxToFid( index ), feat );

    QgsVectorLayerEditBuffer *editBuffer = mFilterModel->layer()->editBuffer();

    if ( editBuffer )
    {
      if ( editBuffer->isFeatureAdded( feat.id() ) )
      {
        featInfo.isNew = true;
      }
      if ( editBuffer->isFeatureAttributesChanged( feat.id() ) )
      {
        featInfo.isEdited = true;
      }
    }

    return QVariant::fromValue( featInfo );
  }
  else if ( role == FeatureRole )
  {
    QgsFeature feat;

    mFilterModel->layerCache()->featureAtId( idxToFid( index ), feat );

    return QVariant::fromValue( feat );
  }
  else if ( role == Qt::TextAlignmentRole )
  {
    return Qt::AlignLeft;
  }

  if ( role == Qt::BackgroundColorRole
       || role == Qt::TextColorRole
       || role == Qt::DecorationRole
       || role == Qt::FontRole )
  {
    QgsVectorLayer *layer = mFilterModel->layer();
    QgsFeature feat;
    QgsFeatureId fid = idxToFid( index );
    mFilterModel->layerCache()->featureAtId( fid, feat );
    mExpressionContext.setFeature( feat );
    QList<QgsConditionalStyle> styles;

    if ( mRowStylesMap.contains( fid ) )
    {
      styles = mRowStylesMap.value( fid );
    }
    else
    {
      styles = QgsConditionalStyle::matchingConditionalStyles( layer->conditionalStyles()->rowStyles(), QVariant(),  mExpressionContext );
      mRowStylesMap.insert( fid, styles );
    }

    QgsConditionalStyle rowstyle = QgsConditionalStyle::compressStyles( styles );

    if ( mDisplayExpression.isField() )
    {
      QString fieldName = *mDisplayExpression.referencedColumns().constBegin();
      styles = layer->conditionalStyles()->fieldStyles( fieldName );
      styles = QgsConditionalStyle::matchingConditionalStyles( styles, feat.attribute( fieldName ),  mExpressionContext );
    }

    styles.insert( 0, rowstyle );

    QgsConditionalStyle style = QgsConditionalStyle::compressStyles( styles );

    if ( style.isValid() )
    {
      if ( role == Qt::BackgroundColorRole && style.validBackgroundColor() )
        return style.backgroundColor();
      if ( role == Qt::TextColorRole && style.validTextColor() )
        return style.textColor();
      if ( role == Qt::DecorationRole )
        return style.icon();
      if ( role == Qt::FontRole )
        return style.font();
    }

    return QVariant();
  }

  return sourceModel()->data( mapToSource( index ), role );
}

Qt::ItemFlags QgsFeatureListModel::flags( const QModelIndex &index ) const
{
  if ( mInjectNull && index.row() == 0 )
  {
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
  }
  else
  {
    return sourceModel()->flags( mapToSource( index ) ) & ~Qt::ItemIsEditable;
  }
}

void QgsFeatureListModel::setInjectNull( bool injectNull )
{
  if ( mInjectNull != injectNull )
  {
    emit beginResetModel();
    mInjectNull = injectNull;
    emit endResetModel();
  }
}

bool QgsFeatureListModel::injectNull()
{
  return mInjectNull;
}

QgsAttributeTableModel *QgsFeatureListModel::masterModel()
{
  return mFilterModel->masterModel();
}

bool QgsFeatureListModel::setDisplayExpression( const QString &expression )
{
  QgsExpression exp = QgsExpression( expression );

  exp.prepare( &mExpressionContext );

  if ( exp.hasParserError() )
  {
    mParserErrorString = exp.parserErrorString();
    return false;
  }

  mDisplayExpression = exp;

  emit dataChanged( index( 0, 0 ), index( rowCount() - 1, 0 ) );
  return true;
}

QString QgsFeatureListModel::parserErrorString()
{
  return mParserErrorString;
}

QString QgsFeatureListModel::displayExpression() const
{
  return mDisplayExpression.expression();
}

bool QgsFeatureListModel::featureByIndex( const QModelIndex &index, QgsFeature &feat )
{
  return mFilterModel->layerCache()->featureAtId( idxToFid( index ), feat );
}

void QgsFeatureListModel::onBeginRemoveRows( const QModelIndex &parent, int first, int last )
{
  beginRemoveRows( parent, first, last );
}

void QgsFeatureListModel::onEndRemoveRows( const QModelIndex &parent, int first, int last )
{
  Q_UNUSED( parent )
  Q_UNUSED( first )
  Q_UNUSED( last )
  endRemoveRows();
}

void QgsFeatureListModel::onBeginInsertRows( const QModelIndex &parent, int first, int last )
{
  beginInsertRows( parent, first, last );
}

void QgsFeatureListModel::onEndInsertRows( const QModelIndex &parent, int first, int last )
{
  Q_UNUSED( parent )
  Q_UNUSED( first )
  Q_UNUSED( last )
  endInsertRows();
}

QModelIndex QgsFeatureListModel::mapToMaster( const QModelIndex &proxyIndex ) const
{
  if ( !proxyIndex.isValid() )
    return QModelIndex();

  int offset = mInjectNull ? 1 : 0;

  return mFilterModel->mapToMaster( mFilterModel->index( proxyIndex.row() - offset, proxyIndex.column() ) );
}

QModelIndex QgsFeatureListModel::mapFromMaster( const QModelIndex &sourceIndex ) const
{
  if ( !sourceIndex.isValid() )
    return QModelIndex();

  int offset = mInjectNull ? 1 : 0;

  return createIndex( mFilterModel->mapFromMaster( sourceIndex ).row() + offset, 0 );
}

QItemSelection QgsFeatureListModel::mapSelectionFromMaster( const QItemSelection &selection ) const
{
  return mapSelectionFromSource( mFilterModel->mapSelectionFromSource( selection ) );
}

QItemSelection QgsFeatureListModel::mapSelectionToMaster( const QItemSelection &selection ) const
{
  return mFilterModel->mapSelectionToSource( mapSelectionToSource( selection ) );
}

// Override some methods from QAbstractProxyModel, not that interesting

QModelIndex QgsFeatureListModel::mapToSource( const QModelIndex &proxyIndex ) const
{
  if ( !proxyIndex.isValid() )
    return QModelIndex();

  int offset = mInjectNull ? 1 : 0;

  return sourceModel()->index( proxyIndex.row() - offset, proxyIndex.column() );
}

QModelIndex QgsFeatureListModel::mapFromSource( const QModelIndex &sourceIndex ) const
{
  if ( !sourceIndex.isValid() )
    return QModelIndex();

  return createIndex( sourceIndex.row(), 0 );
}

QModelIndex QgsFeatureListModel::index( int row, int column, const QModelIndex &parent ) const
{
  Q_UNUSED( parent )

  return createIndex( row, column );
}

QModelIndex QgsFeatureListModel::parent( const QModelIndex &child ) const
{
  Q_UNUSED( child )
  return QModelIndex();
}

int QgsFeatureListModel::columnCount( const QModelIndex &parent ) const
{
  Q_UNUSED( parent )
  return 1;
}

int QgsFeatureListModel::rowCount( const QModelIndex &parent ) const
{
  Q_UNUSED( parent )

  int offset = mInjectNull ? 1 : 0;

  return sourceModel()->rowCount() + offset;
}

QModelIndex QgsFeatureListModel::fidToIndex( QgsFeatureId fid )
{
  return mapFromMaster( masterModel()->idToIndex( fid ) );
}

QModelIndexList QgsFeatureListModel::fidToIndexList( QgsFeatureId fid )
{
  return QModelIndexList() << fidToIndex( fid );
}

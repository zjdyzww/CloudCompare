//##########################################################################
//#                                                                        #
//#                            CLOUDCOMPARE                                #
//#                                                                        #
//#  This program is free software; you can redistribute it and/or modify  #
//#  it under the terms of the GNU General Public License as published by  #
//#  the Free Software Foundation; version 2 of the License.               #
//#                                                                        #
//#  This program is distributed in the hope that it will be useful,       #
//#  but WITHOUT ANY WARRANTY; without even the implied warranty of        #
//#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         #
//#  GNU General Public License for more details.                          #
//#                                                                        #
//#          COPYRIGHT: EDF R&D / TELECOM ParisTech (ENST-TSI)             #
//#                                                                        #
//##########################################################################

#include "ccSectionExtractionTool.h"

//Local
#include "mainwindow.h"
#include "ccEntityPickerDlg.h"
#include "ccOrthoSectionGenerationDlg.h"
#include "ccSectionExtractionSubDlg.h"

//qCC_db
#include <ccLog.h>
#include <ccPolyline.h>
#include <ccGenericPointCloud.h>
#include <ccPointCloud.h>
#include <ccHObjectCaster.h>
#include <ccProgressDialog.h>

//qCC_gl
#include <ccGLWindow.h>

//CCLib
#include <ReferenceCloud.h>

//Qt
#include <QMessageBox>
#include <QMdiSubWindow>

//System
#include <assert.h>

//default parameters
static const colorType* s_defaultPolylineColor         = ccColor::yellow;
static const colorType* s_defaultEditedPolylineColor   = ccColor::green;
static const colorType* s_defaultSelectedPolylineColor = ccColor::red;
static const int        s_defaultPolylineWidth         = 1;
static const int        s_defaultSelectedPolylineWidth = 3;

ccSectionExtractionTool::ccSectionExtractionTool(QWidget* parent)
	: ccOverlayDialog(parent)
	, Ui::SectionExtractionDlg()
	, m_selectedPoly(0)
	, m_state(0)
	, m_editedPoly(0)
	, m_editedPolyVertices(0)
{
	setupUi(this);
	setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);

	connect(undoToolButton,						SIGNAL(clicked()),					this,	SLOT(undo()));
	connect(validToolButton,					SIGNAL(clicked()),					this,	SLOT(apply()));
	connect(polylineToolButton,					SIGNAL(toggled(bool)),				this,	SLOT(enableSectionEditingMode(bool)));
	connect(importFromDBToolButton,				SIGNAL(clicked()),					this,	SLOT(doImportPolylinesFromDB()));
	connect(vertAxisComboBox,					SIGNAL(currentIndexChanged(int)),	this,	SLOT(setVertDimension(int)));

	connect(generateOrthoSectionsToolButton,	SIGNAL(clicked()),					this,	SLOT(generateOrthoSections()));
	connect(extractPointsToolButton,			SIGNAL(clicked()),					this,	SLOT(extractPoints()));
	connect(exportSectionsToolButton,			SIGNAL(clicked()),					this,	SLOT(exportSections()));

	//add shortcuts
	addOverridenShortcut(Qt::Key_Space);  //space bar for the "pause" button
	addOverridenShortcut(Qt::Key_Escape); //cancel current polyline edition
	addOverridenShortcut(Qt::Key_Delete); //delete key to delete the selected polyline

	connect(this, SIGNAL(shortcutTriggered(int)), this, SLOT(onShortcutTriggered(int)));
}

ccSectionExtractionTool::~ccSectionExtractionTool()
{
	if (m_editedPoly)
	{
		if (m_associatedWin)
			m_associatedWin->removeFromOwnDB(m_editedPoly);
		delete m_editedPoly;
		m_editedPoly = 0;
	}
}

void ccSectionExtractionTool::setVertDimension(int dim)
{
	assert(dim >=0 && dim < 3);
	if (!m_associatedWin)
		return;

	switch(dim)
	{
	case 0:
		m_associatedWin->setView(CC_RIGHT_VIEW);
		break;
	case 1:
		m_associatedWin->setView(CC_FRONT_VIEW);
		break;
	case 2:
	default:
		m_associatedWin->setView(CC_TOP_VIEW);
		break;
	}
	m_associatedWin->updateConstellationCenterAndZoom();
}

void ccSectionExtractionTool::onShortcutTriggered(int key)
{
 	switch(key)
	{
	case Qt::Key_Space:
		polylineToolButton->toggle();
		return;

	case Qt::Key_Escape:
		cancelCurrentPolyline();
		return;

	case Qt::Key_Delete:
		deleteSelectedPolyline();
		return;

	default:
		//nothing to do
		break;
	}
}

bool ccSectionExtractionTool::linkWith(ccGLWindow* win)
{
	ccGLWindow* oldWin = m_associatedWin;

	if (!ccOverlayDialog::linkWith(win))
		return false;

	selectPolyline(0);

	if (oldWin)
	{
		disconnect(m_associatedWin, SIGNAL(leftButtonClicked(int,int)), this, SLOT(addPointToPolyline(int,int)));
		disconnect(m_associatedWin, SIGNAL(rightButtonClicked(int,int)), this, SLOT(closePolyLine(int,int)));
		disconnect(m_associatedWin, SIGNAL(mouseMoved(int,int,Qt::MouseButtons)), this, SLOT(updatePolyLine(int,int,Qt::MouseButtons)));
		disconnect(m_associatedWin, SIGNAL(buttonReleased()), this, SLOT(closeRectangle()));
		disconnect(m_associatedWin, SIGNAL(entitySelectionChanged(int)), this, SLOT(entitySelected(int)));

		//restore sections original display
		{
			for (SectionPool::iterator it = m_sections.begin(); it != m_sections.end(); ++it)
			{
				Section& section = *it;
				if (section.entity)
				{
					if (!section.isInDB)
						oldWin->removeFromOwnDB(section.entity);
					section.entity->setDisplay(section.originalDisplay);
				}
			}
		}
		//Restore clouds original display
		{
			for (CloudPool::iterator it = m_clouds.begin(); it != m_clouds.end(); ++it)
			{
				Cloud& cloud = *it;
				if (cloud.entity)
				{
					if (!cloud.isInDB)
						oldWin->removeFromOwnDB(cloud.entity);
					cloud.entity->setDisplay(cloud.originalDisplay);
				}
			}
		}

		if (m_editedPoly)
			m_editedPoly->setDisplay(0);

		//update view direction
		setVertDimension(vertAxisComboBox->currentIndex());

		//auto-close formerly associated window
		if (MainWindow::TheInstance())
		{
			QMdiSubWindow* subWindow = MainWindow::TheInstance()->getMDISubWindow(oldWin);
			if (subWindow)
				subWindow->close();
		}
	}
	
	if (m_associatedWin)
	{
		connect(m_associatedWin, SIGNAL(leftButtonClicked(int,int)), this, SLOT(addPointToPolyline(int,int)));
		connect(m_associatedWin, SIGNAL(rightButtonClicked(int,int)), this, SLOT(closePolyLine(int,int)));
		connect(m_associatedWin, SIGNAL(mouseMoved(int,int,Qt::MouseButtons)), this, SLOT(updatePolyLine(int,int,Qt::MouseButtons)));
		connect(m_associatedWin, SIGNAL(buttonReleased()), this, SLOT(closeRectangle()));
		connect(m_associatedWin, SIGNAL(entitySelectionChanged(int)), this, SLOT(entitySelected(int)));

		//import sections in current display
		{
			for (SectionPool::iterator it = m_sections.begin(); it != m_sections.end(); ++it)
			{
				Section& section = *it;
				if (section.entity)
				{
					section.originalDisplay = section.entity->getDisplay();
					section.entity->setDisplay(m_associatedWin);
					if (!section.isInDB)
						m_associatedWin->addToOwnDB(section.entity);
				}
			}
		}
		//import clouds in current display
		{
			for (CloudPool::iterator it = m_clouds.begin(); it != m_clouds.end(); ++it)
			{
				Cloud& cloud = *it;
				if (cloud.entity)
				{
					cloud.originalDisplay = cloud.entity->getDisplay();
					cloud.entity->setDisplay(m_associatedWin);
					if (!cloud.isInDB)
						m_associatedWin->addToOwnDB(cloud.entity);
				}
			}
		}

		if (m_editedPoly)
			m_editedPoly->setDisplay(m_associatedWin);
	}

	return true;
}

void ccSectionExtractionTool::selectPolyline(Section* poly, bool autoRefreshDisplay/*=true*/)
{
	bool redraw = false;

	//deselect previously selected polyline
	if (m_selectedPoly && m_selectedPoly->entity)
	{
		m_selectedPoly->entity->showColors(true);
		m_selectedPoly->entity->setColor(s_defaultPolylineColor);
		m_selectedPoly->entity->setWidth(s_defaultPolylineWidth);
		redraw = true;
	}

	m_selectedPoly = poly;
	
	//select new polyline (if any)
	if (m_selectedPoly)
	{
		m_selectedPoly->entity->showColors(true);
		m_selectedPoly->entity->setColor(s_defaultSelectedPolylineColor);
		m_selectedPoly->entity->setWidth(s_defaultSelectedPolylineWidth);
		redraw = true;
	}

	if (redraw && autoRefreshDisplay && m_associatedWin)
	{
		m_associatedWin->redraw();
	}

	generateOrthoSectionsToolButton->setEnabled(m_selectedPoly != 0);
}

void ccSectionExtractionTool::releasePolyline(Section* section)
{
	if (section && section->entity)
	{
		if (!section->isInDB)
		{
			//remove from display
			if (m_associatedWin)
				m_associatedWin->removeFromOwnDB(section->entity);
			//delete entity
			delete section->entity;
			section->entity = 0;
		}
		else
		{
			//restore original display and style
			section->entity->showColors(section->backupColorShown);
			section->entity->setColor  (section->backupColor);
			section->entity->setWidth  (section->backupWidth);
			section->entity->setDisplay(section->originalDisplay);
		}
	}
}

void ccSectionExtractionTool::deleteSelectedPolyline()
{
	if (!m_selectedPoly)
		return;

	Section* selectedPoly = m_selectedPoly;

	//deslect polyline before anything
	selectPolyline(0,false);
	
	releasePolyline(selectedPoly);

	//remove the section from the list
	m_sections.removeOne(*selectedPoly);
	m_undoCount.clear();
	undoToolButton->setEnabled(false);

	if (m_associatedWin)
	{
		m_associatedWin->redraw();
	}
}

void ccSectionExtractionTool::entitySelected(int uniqueID)
{
	//look if this unique ID corresponds to an active polyline
	for (SectionPool::iterator it = m_sections.begin(); it != m_sections.end(); ++it)
	{
		Section& section = *it;
		if (section.entity && section.entity->getUniqueID() == uniqueID)
		{
			selectPolyline(&section);
			break;
		}
	}
}

bool ccSectionExtractionTool::start()
{
	assert(!m_editedPolyVertices && !m_editedPoly);

	if (!m_associatedWin)
	{
		ccLog::Warning("[Graphical Segmentation Tool] No associated window!");
		return false;
	}

	//the user must not close this window!
	m_associatedWin->setUnclosable(true);
	m_associatedWin->updateConstellationCenterAndZoom();
	
	enableSectionEditingMode(true);

	return ccOverlayDialog::start();
}

void ccSectionExtractionTool::removeAllEntities()
{
	reset(false);

	//and we remove the remaining clouds (if any)
	{
		for (int i=0; i < m_clouds.size(); ++i)
		{
			Cloud& cloud = m_clouds[i];
			if (cloud.entity)
			{
				assert(cloud.isInDB);
				//restore original display
				cloud.entity->setDisplay(cloud.originalDisplay);
			}
		}
		m_clouds.clear();
	}
}

void ccSectionExtractionTool::undo()
{
	if (m_undoCount.empty())
		return;

	size_t count = 0;
	do
	{
		count = m_undoCount.back();
		m_undoCount.pop_back();
	}
	while (count >= m_sections.size() && !m_undoCount.empty());

	//ask for a confirmation
	if (QMessageBox::question(MainWindow::TheInstance(),"Undo",QString("Remove %1 polylines?").arg(m_sections.size()-count),QMessageBox::Yes,QMessageBox::No) == QMessageBox::No)
	{
		//restore undo stack!
		m_undoCount.push_back(count);
		return;
	}

	selectPolyline(0);

	//we remove all polylines after a given point
	{
		while (m_sections.size() > count)
		{
			Section& section = m_sections.back();
			releasePolyline(&section);
			m_sections.pop_back();
		}
	}

	//update GUI
	exportSectionsToolButton->setEnabled(count != 0);
	extractPointsToolButton->setEnabled(count != 0);
	undoToolButton->setEnabled(!m_undoCount.empty());

	if (m_associatedWin)
		m_associatedWin->redraw();
}

bool ccSectionExtractionTool::reset(bool askForConfirmation/*=true*/)
{
	if (m_sections.empty() && m_clouds.empty())
	{
		//nothing to do
		return true;
	}

	if (askForConfirmation)
	{
		//if we found at least one temporary polyline, we display a confirmation message
		for (SectionPool::iterator it = m_sections.begin(); it != m_sections.end(); ++it)
		{
			Section& section = *it;
			if (section.entity && !section.isInDB)
			{
				if (QMessageBox::question(MainWindow::TheInstance(), "Reset", "You'll lose all manually defined polylines: are you sure?", QMessageBox::Yes, QMessageBox::No) == QMessageBox::No)
					return false;
				else
					break;
			}
		}
	}

	selectPolyline(0);

	//we remove all polylines
	{
		for (SectionPool::iterator it = m_sections.begin(); it != m_sections.end(); ++it)
		{
			Section& section = *it;
			releasePolyline(&section);
		}
		m_sections.clear();
		m_undoCount.clear();
		undoToolButton->setEnabled(false);
		exportSectionsToolButton->setEnabled(false);
		extractPointsToolButton->setEnabled(false);
	}

	//and we remove only temporary clouds
	{
		for (int i=0; i < m_clouds.size(); )
		{
			Cloud& cloud = m_clouds[i];
			if (cloud.entity && !cloud.isInDB)
			{
				if (m_associatedWin)
					m_associatedWin->removeFromOwnDB(cloud.entity);
				delete cloud.entity;
				cloud.entity = 0;

				m_clouds.removeAt(i);
			}
			else
			{
				++i;
			}
		}
	}

	if (m_associatedWin)
		m_associatedWin->redraw();

	return true;
}

void ccSectionExtractionTool::stop(bool accepted)
{
	if (m_editedPoly)
	{
		if (m_associatedWin)
			m_associatedWin->removeFromOwnDB(m_editedPoly);
		delete m_editedPoly;
		m_editedPoly = 0;
	}

	enableSectionEditingMode(false);
	reset(true);

	if (m_associatedWin)
	{
		m_associatedWin->setInteractionMode(ccGLWindow::TRANSFORM_CAMERA);
		m_associatedWin->setPickingMode(ccGLWindow::DEFAULT_PICKING);
		m_associatedWin->setUnclosable(false);
	}

	ccOverlayDialog::stop(accepted);
}

bool ccSectionExtractionTool::addPolyline(ccPolyline* inputPoly, bool alreadyInDB/*=true*/)
{
	if (!inputPoly)
	{
		assert(false);
		return false;
	}

	for (SectionPool::iterator it = m_sections.begin(); it != m_sections.end(); ++it)
	{
		Section& section = *it;
		if (section.entity == inputPoly)
		{
			//cloud already in DB
			return false;
		}
	}

	//convert the polyline to 3D mode if necessary
	if (inputPoly->is2DMode())
	{
		//viewing parameters (for conversion from 2D to 3D)
		const double* MM = m_associatedWin->getModelViewMatd(); //viewMat
		const double* MP = m_associatedWin->getProjectionMatd(); //projMat
		const GLdouble half_w = static_cast<GLdouble>(m_associatedWin->width())/2;
		const GLdouble half_h = static_cast<GLdouble>(m_associatedWin->height())/2;
		int VP[4];
		m_associatedWin->getViewportArray(VP);

		//get default altitude from the cloud(s) bouding-box
		PointCoordinateType defaultZ = 0;
		int vertDim = vertAxisComboBox->currentIndex();
		assert(vertDim >= 0 && vertDim < 3);
		{
			bool validZ = false;
			for (int i=0; i<m_clouds.size(); ++i)
			{
				if (m_clouds[i].entity)
				{
					PointCoordinateType maxZ = m_clouds.front().entity->getBB().maxCorner()[vertDim];
					if (validZ)
					{
						defaultZ = std::max(defaultZ, maxZ);
					}
					else
					{
						validZ = true;
						defaultZ = maxZ;
					}
				}
			}
		}

		//duplicate polyline
		ccPolyline* duplicatePoly = new ccPolyline(0);
		ccPointCloud* duplicateVertices = 0;
		if (duplicatePoly->initWith(duplicateVertices,*inputPoly))
		{
			assert(duplicateVertices);
			for (unsigned i=0; i<duplicateVertices->size(); ++i)
			{
				CCVector3& P = const_cast<CCVector3&>(*duplicateVertices->getPoint(i));
				GLdouble xp,yp,zp;
				gluUnProject(half_w+P.x,half_h+P.y,0/*P.z*/,MM,MP,VP,&xp,&yp,&zp);
				P.x = static_cast<PointCoordinateType>(xp);
				P.y = static_cast<PointCoordinateType>(yp);
				P.z = static_cast<PointCoordinateType>(zp);
				P.u[vertDim] = defaultZ;
			}

			duplicateVertices->invalidateBoundingBox();
			duplicatePoly->set2DMode(false);
			duplicatePoly->setDisplay(inputPoly->getDisplay());

			if (!alreadyInDB)
				delete inputPoly;
			else
				alreadyInDB = false;
			inputPoly = duplicatePoly;
		}
		else
		{
			delete duplicatePoly;
			duplicatePoly = 0;
			
			ccLog::Error("Not enough memory to import polyline!");
			return false;
		}
	}

	//add polyline to the 'sections' set
	//(all its parameters will be backuped!)
	m_sections.push_back(Section(inputPoly,alreadyInDB));
	exportSectionsToolButton->setEnabled(true);
	extractPointsToolButton->setEnabled(true);

	//apply default look
	inputPoly->showColors(true);
	inputPoly->setColor(s_defaultPolylineColor);
	inputPoly->setWidth(s_defaultPolylineWidth);

	//add to display
	if (m_associatedWin)
	{
		inputPoly->setDisplay(m_associatedWin);
		if (!alreadyInDB)
			m_associatedWin->addToOwnDB(inputPoly);
	}

	return true;
}

bool ccSectionExtractionTool::addCloud(ccGenericPointCloud* inputCloud, bool alreadyInDB/*=true*/)
{
	assert(inputCloud);

	for (CloudPool::iterator it = m_clouds.begin(); it != m_clouds.end(); ++it)
	{
		Cloud& cloud = *it;
		if (cloud.entity == inputCloud)
		{
			//cloud already in DB
			return false;
		}
	}

	m_clouds.push_back(Cloud(inputCloud,alreadyInDB));
	if (m_associatedWin)
	{
		inputCloud->setDisplay(m_associatedWin);
		if (!alreadyInDB)
			m_associatedWin->addToOwnDB(inputCloud);
	}

	return true;
}

//CCVector3 ccSectionExtractionTool::project2Dto3D(int x, int y) const
//{
//	//get current display parameters
//	const double* MM = m_associatedWin->getModelViewMatd(); //viewMat
//	const double* MP = m_associatedWin->getProjectionMatd(); //projMat
//	const GLdouble half_w = static_cast<GLdouble>(m_associatedWin->width())/2;
//	const GLdouble half_h = static_cast<GLdouble>(m_associatedWin->height())/2;
//	int VP[4];
//	m_associatedWin->getViewportArray(VP);
//
//	GLdouble xp,yp,zp;
//	gluUnProject(half_w+x,half_h+y,0/*z*/,MM,MP,VP,&xp,&yp,&zp);
//
//	return CCVector3(	static_cast<PointCoordinateType>(xp),
//						static_cast<PointCoordinateType>(yp),
//						static_cast<PointCoordinateType>(zp) );
//}

void ccSectionExtractionTool::updatePolyLine(int x, int y, Qt::MouseButtons buttons)
{
	if (!m_associatedWin)
		return;

	//process not started yet?
	if ((m_state & RUNNING) == 0)
		return;

	if (!m_editedPoly)
		return;

	unsigned vertCount = m_editedPolyVertices->size();
	if (vertCount < 2)
		return;
	
	CCVector3 P = CCVector3(static_cast<PointCoordinateType>(x),
							static_cast<PointCoordinateType>(y),
							0);

	//we replace last point by the current one
	CCVector3* lastP = const_cast<CCVector3*>(m_editedPolyVertices->getPointPersistentPtr(vertCount-1));
	*lastP = P;

	if (m_associatedWin)
		m_associatedWin->updateGL();
}

void ccSectionExtractionTool::addPointToPolyline(int x, int y)
{
	if ((m_state & STARTED) == 0)
		return;

	if (!m_editedPoly)
	{
		assert(!m_editedPolyVertices);
		m_editedPolyVertices = new ccPointCloud("vertices");
		m_editedPoly = new ccPolyline(m_editedPolyVertices);
		m_editedPoly->setForeground(true);
		m_editedPoly->setColor(s_defaultEditedPolylineColor);
		m_editedPoly->showColors(true);
		m_editedPoly->set2DMode(true);
		m_editedPoly->addChild(m_editedPolyVertices);
		if (m_associatedWin)
			m_associatedWin->addToOwnDB(m_editedPoly);
	}

	unsigned vertCount = m_editedPolyVertices->size();

	//clicked point (2D)
	CCVector3 P = CCVector3(static_cast<PointCoordinateType>(x),
							static_cast<PointCoordinateType>(y),
							0);

	//start new polyline?
	if (((m_state & RUNNING) == 0) || vertCount == 0)
	{
		//reset state
		m_state = (STARTED | RUNNING);
		//reset polyline
		m_editedPolyVertices->clear();
		if (!m_editedPolyVertices->reserve(2))
		{
			ccLog::Error("Out of memory!");
			return;
		}
		//we add the same point twice (the last point will be used for display only)
		m_editedPolyVertices->addPoint(P);
		m_editedPolyVertices->addPoint(P);
		m_editedPoly->clear();
		if (!m_editedPoly->addPointIndex(0,2))
		{
			ccLog::Error("Out of memory!");
			return;
		}
	}
	else //next points
	{
		if (!m_editedPolyVertices->reserve(vertCount+1))
		{
			ccLog::Error("Out of memory!");
			return;
		}

		//we replace last point by the current one
		CCVector3* lastP = const_cast<CCVector3*>(m_editedPolyVertices->getPointPersistentPtr(vertCount-1));
		*lastP = P;
		//and add a new (equivalent) one
		m_editedPolyVertices->addPoint(P);
		if (!m_editedPoly->addPointIndex(vertCount))
		{
			ccLog::Error("Out of memory!");
			return;
		}
	}

	if (m_associatedWin)
		m_associatedWin->updateGL();
}

void ccSectionExtractionTool::closePolyLine(int, int)
{
	//only in RUNNING mode
	if ((m_state & RUNNING) == 0 || !m_editedPoly)
		return;

	assert(m_editedPoly);
	unsigned vertCount = m_editedPoly->size();
	if (vertCount < 3)
	{
		m_editedPoly->clear();
		m_editedPolyVertices->clear();
	}
	else
	{
		//remove last point!
		m_editedPoly->resize(vertCount-1); //can't fail --> smaller

		//remove polyline from the 'temporary' world
		if (m_associatedWin)
			m_associatedWin->removeFromOwnDB(m_editedPoly);
		//set default display style
		m_editedPoly->showColors(true);
		m_editedPoly->setColor(s_defaultPolylineColor);
		m_editedPoly->setWidth(s_defaultPolylineWidth);
		if (!m_clouds.isEmpty())
			m_editedPoly->setDisplay_recursive(m_clouds.front().originalDisplay); //set the same 'default' display as the cloud
		m_editedPoly->setName(QString("Polyline #%1").arg(m_sections.size()+1));
		//save polyline
		if (!addPolyline(m_editedPoly,false))
		{
			//if something went wrong, we have to remove the polyline manually
			delete m_editedPoly;
		}
		m_editedPoly = 0;
		m_editedPolyVertices = 0;
	}

	//stop
	m_state &= (~RUNNING);

	if (m_associatedWin)
		m_associatedWin->updateGL();
}

void ccSectionExtractionTool::cancelCurrentPolyline()
{
	if (	(m_state & STARTED) == 0
		||	!m_editedPoly)
	{
		return;
	}

	assert(m_editedPolyVertices);
	m_editedPoly->clear();
	m_editedPolyVertices->clear();

	//stop
	m_state &= (~RUNNING);

	if (m_associatedWin)
		m_associatedWin->redraw();
}

void ccSectionExtractionTool::enableSectionEditingMode(bool state)
{
	if (!m_associatedWin)
		return;

	if (!state/*=activate pause mode*/)
	{
		//select the last polyline by default (if any)
		if (!m_sections.empty() && !m_sections.back().isInDB)
			selectPolyline(&m_sections.back());

		m_state = PAUSED;
		
		if (m_editedPoly && m_editedPolyVertices)
		{
			m_editedPoly->clear();
			m_editedPolyVertices->clear();
		}
		m_associatedWin->setInteractionMode(ccGLWindow::PAN_ONLY);
		m_associatedWin->displayNewMessage(QString(),ccGLWindow::UPPER_CENTER_MESSAGE,false,0,ccGLWindow::MANUAL_SEGMENTATION_MESSAGE);
		m_associatedWin->setPickingMode(ccGLWindow::ENTITY_PICKING); //to be able to select polylines!
	}
	else
	{
		//deselect all currently selected polylines
		selectPolyline(0);

		//set new 'undo' step
		addUndoStep();

		m_state = STARTED;
		
		m_associatedWin->setPickingMode(ccGLWindow::NO_PICKING);
		m_associatedWin->setInteractionMode(ccGLWindow::SEGMENT_ENTITY);
		m_associatedWin->displayNewMessage("Section edition mode",ccGLWindow::UPPER_CENTER_MESSAGE,false,3600,ccGLWindow::MANUAL_SEGMENTATION_MESSAGE);
		m_associatedWin->displayNewMessage("Left click: add section points / Right click: stop",ccGLWindow::UPPER_CENTER_MESSAGE,true,3600,ccGLWindow::MANUAL_SEGMENTATION_MESSAGE);
	}

	//update mini-GUI
	polylineToolButton->blockSignals(true);
	polylineToolButton->setChecked(state);
	frame->setEnabled(!state);
	polylineToolButton->blockSignals(false);

	m_associatedWin->redraw();
}

void ccSectionExtractionTool::addUndoStep()
{
	if (m_undoCount.empty() || (m_undoCount.back() < m_sections.size()))
	{
		m_undoCount.push_back(m_sections.size());
		undoToolButton->setEnabled(true);
	}
}

void ccSectionExtractionTool::doImportPolylinesFromDB()
{
	MainWindow* mainWindow = MainWindow::TheInstance();
	if (!mainWindow)
		return;

	ccHObject* root = mainWindow->dbRootObject();
	ccHObject::Container polylines;
	if (root)
	{
		root->filterChildren(polylines,true,CC_TYPES::POLY_LINE);
	}

	if (!polylines.empty())
	{
		ccEntityPickerDlg epDlg(polylines,true,0,this);
		if (!epDlg.exec())
			return;

		//set new 'undo' step
		addUndoStep();

		enableSectionEditingMode(false);
		std::vector<int> indexes;
		epDlg.getSelectedIndexes(indexes);
		for (size_t i=0; i<indexes.size(); ++i)
		{
			int index = indexes[i];
			assert(index >= 0 && index < static_cast<int>(polylines.size()));
			assert(polylines[index]->isA(CC_TYPES::POLY_LINE));
			ccPolyline* poly = static_cast<ccPolyline*>(polylines[index]);
			addPolyline(poly,true);
		}
		//auto-select the last one
		if (!m_sections.empty())
			selectPolyline(&(m_sections.back()));
		if (m_associatedWin)
			m_associatedWin->redraw();
	}
	else
	{
		ccLog::Error("No polyline in DB!");
	}
}

void ccSectionExtractionTool::apply()
{
	if (!reset(true))
		return;
	
	stop(true);
}

static double s_orthoSectionWidth = -1.0;
static double s_orthoSectionStep  = -1.0;
void ccSectionExtractionTool::generateOrthoSections()
{
	if (!m_selectedPoly)
	{
		ccLog::Warning("[ccSectionExtractionTool] No polyline selected");
		return;
	}

	//compute poyline length
	ccPolyline* poly = m_selectedPoly->entity;
	unsigned vertCount = (poly ? poly->size() : 0);
	if (vertCount < 2)
	{
		ccLog::Warning("[ccSectionExtractionTool] Invalid polyline");
		return;
	}

	PointCoordinateType length = poly->computeLength();

	//display dialog
	ccOrthoSectionGenerationDlg osgDlg(MainWindow::TheInstance());
	osgDlg.setPathLength(length);
	if (s_orthoSectionWidth > 0.0)
		osgDlg.setSectionsWidth(s_orthoSectionWidth);
	if (s_orthoSectionStep > 0.0)
		osgDlg.setGenerationStep(s_orthoSectionStep);

	if (!osgDlg.exec())
		return;

	//set new 'undo' step
	addUndoStep();

	//now generate the orthogonal sections
	s_orthoSectionStep = osgDlg.getGenerationStep();
	s_orthoSectionWidth = osgDlg.getSectionsWidth();

	//normal to the plane
	CCVector3 N(0,0,0);
	int vertDim = vertAxisComboBox->currentIndex();
	assert(vertDim >= 0 && vertDim < 3);
	{
		N.u[vertDim] = 1.0;
	}

	//curvilinear position
	double s = 0;
	//current length
	double l = 0;
	unsigned maxCount = vertCount;
	if (!poly->isClosed())
		maxCount--;
	unsigned polyIndex = 0;
	for (unsigned i=0; i<maxCount; ++i)
	{
		const CCVector3* A = poly->getPoint(i);
		const CCVector3* B = poly->getPoint((i+1) % vertCount);
		CCVector3 AB = (*B-*A);
		AB.u[vertDim] = 0;
		CCVector3 nAB = AB.cross(N);
		nAB.normalize();
		
		double lAB = (*B-*A).norm();
		while (s < l + lAB)
		{
			double s_local = s - l;
			assert(s_local < lAB);

			//create orhogonal polyline
			ccPointCloud* vertices = new ccPointCloud("vertices");
			ccPolyline* orthoPoly = new ccPolyline(vertices);
			orthoPoly->addChild(vertices);
			if (vertices->reserve(2) && orthoPoly->reserve(2))
			{
				//intersection point
				CCVector3 I = *A + AB * (s_local / lAB);
				CCVector3 I1 = I + nAB * static_cast<PointCoordinateType>(s_orthoSectionWidth/2);
				CCVector3 I2 = I - nAB * static_cast<PointCoordinateType>(s_orthoSectionWidth/2);

				vertices->addPoint(I1);
				orthoPoly->addPointIndex(0);
				vertices->addPoint(I2);
				orthoPoly->addPointIndex(1);

				orthoPoly->setClosed(false);
				orthoPoly->set2DMode(false);

				//set default display style
				vertices->setVisible(false);
				orthoPoly->showColors(true);
				orthoPoly->setColor(s_defaultPolylineColor);
				orthoPoly->setWidth(s_defaultPolylineWidth);
				if (!m_clouds.isEmpty())
					orthoPoly->setDisplay_recursive(m_clouds.front().originalDisplay); //set the same 'default' display as the cloud
				orthoPoly->setName(QString("%1.%2").arg(poly->getName()).arg(++polyIndex));

				if (!addPolyline(orthoPoly,false))
				{
					delete orthoPoly;
					orthoPoly = 0;
				}
			}
			else
			{
				delete orthoPoly;
				ccLog::Error("Not enough memory!");
				i = vertCount;
				break;
			}

			s += s_orthoSectionStep;
		}

		l += lAB;
	}

	if (m_associatedWin)
		m_associatedWin->redraw();
}

static unsigned s_polyExportGroupID = 0;
void ccSectionExtractionTool::exportSections()
{
	if (m_sections.empty())
		return;

	//we only export 'temporary' objects
	unsigned exportCount = 0;
	{
		for (SectionPool::iterator it = m_sections.begin(); it != m_sections.end(); ++it)
		{
			Section& section = *it;
			if (section.entity && !section.isInDB)
				++exportCount;
		}
	}

	if (!exportCount)
	{
		//nothing to do
		ccLog::Warning("[ccSectionExtractionTool] All active sections are already in DB");
		return;
	}

	MainWindow* mainWin = MainWindow::TheInstance();
	ccHObject* root = mainWin ? mainWin->dbRootObject() : 0;
	if (!root)
	{
		ccLog::Warning("[ccSectionExtractionTool] Internal error (no MainWindow or DB?!)");
		assert(false);
		return;
	}
	
	ccHObject* destEntity = (s_polyExportGroupID != 0 ? root->find(s_polyExportGroupID) : 0);
	if (!destEntity)
	{
		destEntity = new ccHObject("Exported sections");
		mainWin->addToDB(destEntity);
		s_polyExportGroupID = destEntity->getUniqueID();
	}
	assert(destEntity);

	//export entites
	{
		for (SectionPool::iterator it = m_sections.begin(); it != m_sections.end(); ++it)
		{
			Section& section = *it;
			if (section.entity && !section.isInDB)
			{
				destEntity->addChild(section.entity);
				section.isInDB = true;
				mainWin->addToDB(section.entity,false,false);
			}
		}
	}

	ccLog::Print(QString("[ccSectionExtractionTool] %1 sections exported").arg(exportCount));
}


static unsigned s_cloudExportGroupID = 0;
bool ccSectionExtractionTool::extractSectionCloud(const std::vector<CCLib::ReferenceCloud*>& refClouds, unsigned sectionIndex)
{
	ccPointCloud* sectionCloud = 0;
	Cloud* firstValidCloud = 0;
	for (int i=0; i<static_cast<int>(refClouds.size()); ++i)
	{
		if (!refClouds[i])
			continue;
		assert(m_clouds[i].entity); //a valid ref. cloud must have a valid counterpart!
		if (!firstValidCloud)
			firstValidCloud = &(m_clouds[i]);
		
		//extract part/section from each cloud
		ccPointCloud* part = 0;
		{
			//if the cloud is a ccPointCloud, we can keep a lot more information
			//when extracting the section cloud
			ccPointCloud* pc = dynamic_cast<ccPointCloud*>(m_clouds[i].entity);
			if (pc)
				part = pc->partialClone(refClouds[i]);
			else
				part = ccPointCloud::From(refClouds[i]);
		}

		if (part)
		{
			if (refClouds.size() == 1)
			{
				//we simply use this 'part' cloud as the section cloud
				sectionCloud = part;
			}
			else
			{
				//fuse it with the global cloud
				unsigned cloudSizeBefore = sectionCloud->size();
				unsigned partSize        = part->size();
				sectionCloud->append(part,cloudSizeBefore,true);

				//don't need it anymore
				delete part;
				part = 0;
				//check that it actually worked!
				if (sectionCloud->size() != cloudSizeBefore + partSize)
				{
					//not enough memory
					ccLog::Warning("[ccSectionExtractionTool] Not enough memory");
					if (sectionCloud)
						delete sectionCloud;
					return false;
				}
			}
		}
		else
		{
			//not enough memory
			ccLog::Warning("[ccSectionExtractionTool] Not enough memory");
			if (sectionCloud)
				delete sectionCloud;
			return false;
		}
	}

	if (sectionCloud)
	{
		MainWindow* mainWin = MainWindow::TheInstance();
		ccHObject* root = mainWin ? mainWin->dbRootObject() : 0;
		if (!root)
		{
			assert(false);
			ccLog::Warning("[ccSectionExtractionTool] Internal error (no MainWindow or DB?!)");
			delete sectionCloud;
			return false;
		}

		//create output group if necessary
		ccHObject* destEntity = (s_cloudExportGroupID != 0 ? root->find(s_cloudExportGroupID) : 0);
		if (!destEntity)
		{
			destEntity = new ccHObject("Extracted section clouds");
			mainWin->addToDB(destEntity);
			s_cloudExportGroupID = destEntity->getUniqueID();
		}
		assert(destEntity);

		sectionCloud->setName(QString("Section cloud #%1").arg(sectionIndex+1));

		//add to main DB
		assert(firstValidCloud);
		if (firstValidCloud)
			sectionCloud->setDisplay_recursive(firstValidCloud->originalDisplay);
		destEntity->addChild(sectionCloud);
		MainWindow::TheInstance()->addToDB(sectionCloud,false,false);
	}

	return true;
}

static double s_defaultSectionThickness = -1.0;
static double s_contourMaxEdgeLength = 0;
static bool s_extractSectionsAsClouds = false;
static bool s_extractSectionsAsPolys = true;

void ccSectionExtractionTool::extractPoints()
{
	int sectionCount = m_sections.size();
	if (sectionCount <= 0)
		return;

	//compute loaded clouds bounding-box
	ccBBox box;
	unsigned pointCount = 0;
	{
		for (int i=0; i<m_clouds.size(); ++i)
		{
			if (m_clouds[i].entity)
			{
				box += m_clouds[i].entity->getBB();
				pointCount += m_clouds[i].entity->size();
			}
		}
	}
	
	if (s_defaultSectionThickness <= 0)
	{
		s_defaultSectionThickness = box.getMaxBoxDim() / 500.0;
	}

	//show dialog
	ccSectionExtractionSubDlg sesDlg(MainWindow::TheInstance());
	sesDlg.setSectionThickness(s_defaultSectionThickness);
	sesDlg.setMaxEdgeLength(s_contourMaxEdgeLength);
	sesDlg.doExtractClouds(s_extractSectionsAsClouds);
	sesDlg.doExtractContours(s_extractSectionsAsPolys);

	if (!sesDlg.exec())
		return;

	s_defaultSectionThickness = sesDlg.getSectionThickness();
	s_contourMaxEdgeLength    = sesDlg.getMaxEdgeLength();
	s_extractSectionsAsClouds = sesDlg.extractClouds();
	s_extractSectionsAsPolys  = sesDlg.extractContours();

	//progress dialog
	ccProgressDialog pdlg(true);
	CCLib::NormalizedProgress nprogress(&pdlg,static_cast<unsigned>(sectionCount));
	pdlg.setMethodTitle("Extract sections");
	pdlg.setInfo(qPrintable(QString("Number of sections: %1\nNumber of points: %2").arg(sectionCount).arg(pointCount)));
	pdlg.start();

	PointCoordinateType defaultZ = 0;
	int vertDim = vertAxisComboBox->currentIndex();
	int xDim = (vertDim < 2 ? vertDim+1 : 0);
	int yDim = (xDim    < 2 ? xDim+1    : 0);

	double sectioThicknessSq = s_defaultSectionThickness * s_defaultSectionThickness;
	bool error = false;
	ccHObject* destEntity = 0;

	try
	{
		//for each slice
		for (int s=0; s<m_sections.size(); ++s)
		{
			ccPolyline* poly = m_sections[s].entity;
			if (poly)
			{
				unsigned polyVertCount = poly->size();
				if (polyVertCount < 2)
				{
					assert(false);
					continue;
				}
				unsigned polyMaxCount = poly->isClosed() ? polyVertCount-1 : polyVertCount;

				int cloudCount = m_clouds.size();
				std::vector<CCLib::ReferenceCloud*> refClouds;
				refClouds.resize(cloudCount,0);

				//for each cloud
				for (int c=0; c<cloudCount; ++c)
				{
					ccGenericPointCloud* cloud = m_clouds[c].entity;
					if (cloud)
					{
						CCLib::ReferenceCloud* refCloud = new CCLib::ReferenceCloud(cloud);
						//now test each point and see if it's close to the current polyline (in 2D)
						for (unsigned j=0; j<polyMaxCount; ++j)
						{
							//current polyline segment
							const CCVector3* A = poly->getPoint(j);
							const CCVector3* B = poly->getPoint((j+1) % polyVertCount);
							CCVector2 A2D(A->u[xDim],A->u[yDim]);
							CCVector2 B2D(B->u[xDim],B->u[yDim]);

							CCVector2 u = B2D-A2D;
							PointCoordinateType d = u.norm();
							if (d < ZERO_TOLERANCE)
								continue;
							u /= d;

							//compute dist of each point to this poly's segment
							for (unsigned i=0; i<cloud->size(); ++i)
							{
								const CCVector3* P = cloud->getPoint(i);
								CCVector2 P2D(P->u[xDim],P->u[yDim]);
								CCVector2 AP2D = P2D-A2D;
								
								//longitudinal 'distance'
								PointCoordinateType ps = u.dot(AP2D);
								if (ps < 0 || ps > d)
									continue;

								//orthogonal distance
								PointCoordinateType h = (AP2D - u*ps).norm2();
								if (h <= sectioThicknessSq)
								{
									if (refCloud->size() == refCloud->capacity())
									{
										if (!refCloud->reserve(std::max<unsigned>(refCloud->size()/2,1)*2))
										{
											//not enough memory
											ccLog::Warning("[ccSectionExtractionTool] Not enough memory");
											error = true;
											break;
										}
									}
									refCloud->addPointIndex(i);
								}
							}

							if (error)
								break;
						} //for (unsigned j=0; j<polyMaxCount; ++j)

						if (error || refCloud->size() == 0)
						{
							delete refCloud;
							refCloud = 0;
						}
						else
						{
							refClouds[c] = refCloud;
						}

					} //if (cloud)

					if (error)
						break;
				
				} //for (int c=0; c<cloudCount; ++c)

				if (!error)
				{
					assert(static_cast<int>(refClouds.size()) == cloudCount);

					//Extract sections as (polyline) contours
					if (/*!error && */s_extractSectionsAsPolys)
					{
						//error = ...
					}
					
					//Extract sections as clouds
					if (!error && s_extractSectionsAsClouds)
					{
						error = !extractSectionCloud(refClouds,s);
					}
				}
				
				//release memory
				{
					for (size_t i=0; i<refClouds.size(); ++i)
					{
						if (refClouds[i])
							delete refClouds[i];
						refClouds[i] = 0;
					}
				}
			} //if (poly)

			if (error)
				break;
		} //for (int s=0; s<m_sections.size(); ++s)
	}
	catch(std::bad_alloc)
	{
		error = true;
	}

	if (error)
		ccLog::Error("An error occurred (see console)");
}

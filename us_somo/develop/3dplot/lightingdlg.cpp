#include <qlayout.h>
#include <q3frame.h>
#include <qslider.h>
#include <qcheckbox.h>
#include <qpushbutton.h>
//Added by qt3to4:
#include <Q3GridLayout>

#include "lightingdlg.h"

// For some reson WIN32 is not picking up M_PI from math.h...

#ifndef M_PI  
#define M_PI       3.14159265358979323846
#endif

using namespace Qwt3D;

class Sphere : public ParametricSurface
{
public:

   Sphere(SurfacePlot& pw)
      :ParametricSurface(pw)
   {
      setMesh(41,31);
      setDomain(0,2*M_PI,0,M_PI);
      setPeriodic(false,false);
   }


   Triple operator()(double u, double v)
   {
      double x,y,z;
      double r = 1;
      x = r*cos(u)*sin(v);
      y = r*sin(u)*sin(v);
      z = r*cos(v);
      return Triple(x,y,z);
   }
};


/////////////////////////////////////////////////////////////////
//
//   Plot
//
/////////////////////////////////////////////////////////////////

Plot::Plot(QWidget *parent)
   : SurfacePlot(parent)
{
   setTitle("A Simple SurfacePlot Demonstration");
  
   Sphere sphere(*this);
   sphere.create();

   reset();  
   assignMouse(Qt::LeftButton,
               Qt::RightButton,
               Qt::LeftButton,
               Qt::NoButton,
               Qt::NoButton,
               Qt::NoButton,
               Qt::NoButton,
               Qt::NoButton,
               Qt::NoButton
               );

   stick = (Pointer*)addEnrichment(Pointer(0.05));
   stick->setPos(0,0,1);
}

void Plot::reset()
{
   makeCurrent();
   setRotation(0,0,0);
   setTitle("Use your mouse buttons and keyboard");
   setTitleFont("Arial", 8, QFont::Bold);
   setTitleColor(RGBA(0.9,0.9,0.9));
   setSmoothMesh(true);
   setZoom(0.9);
   setCoordinateStyle(NOCOORD);
   setMeshColor(RGBA(0.6,0.6,0.6,0.3));
   setPlotStyle(FILLEDMESH);
   setBackgroundColor(RGBA(0,0,0));

   updateData();
}

/////////////////////////////////////////////////////////////////
//
//   Pointer
//
/////////////////////////////////////////////////////////////////


Pointer::Pointer(double rad)
{
   configure(rad);
}

Pointer::~Pointer()
{
}

void Pointer::configure(double rad)
{
   plot = 0;
  
   radius_ = rad;
}

void Pointer::drawBegin()
{
#ifndef MAC
   GLint mode;
   glGetIntegerv(GL_MATRIX_MODE, &mode);
   glMatrixMode( GL_MODELVIEW );
   glPushMatrix();
  
   glColor3d(1,0,0);
   glBegin(GL_LINES);
   glVertex3d(pos_.x, pos_.y, pos_.z);
   glVertex3d(0, 0, 0);
   glEnd();

   glPopMatrix();
   glMatrixMode(mode);
#endif
}


LightingDlg::LightingDlg(QWidget *parent)
   :lightingdlgbaseBase(parent)
{
   Q3GridLayout *grid = new Q3GridLayout( frame, 0, 0 );

   dataPlot = 0;
  
   plot = new Plot(frame);
   plot->updateData();

   grid->addWidget( plot, 0, 0 );

   connect( stdlight, SIGNAL( clicked() ), this, SLOT( reset() ) );
   connect( distSL, SIGNAL(valueChanged(int)), this, SLOT(setDistance(int)) );
   connect( emissSL, SIGNAL(valueChanged(int)), this, SLOT(setEmission(int)) );
   connect( ambdiffSL, SIGNAL(valueChanged(int)), this, SLOT(setDiff(int)) );
   connect( specSL, SIGNAL(valueChanged(int)), this, SLOT(setSpec(int)) );
   connect( shinSL, SIGNAL(valueChanged(int)), this, SLOT(setShin(int)) );
   connect( plot, SIGNAL(rotationChanged(double, double, double)), this, SLOT(setRotation(double, double, double)) );
}

LightingDlg::~LightingDlg()
{
   delete plot;
}

void LightingDlg::setEmission(int val)
{
   if (!dataPlot)
      return;
   dataPlot->setMaterialComponent(GL_EMISSION, val / 100.);
   dataPlot->updateGL();
}
void LightingDlg::setDiff(int val)
{
   if (!dataPlot)
      return;
   dataPlot->setLightComponent(GL_DIFFUSE, val / 100.);
   dataPlot->updateGL();
}
void LightingDlg::setSpec(int val)
{
   if (!dataPlot)
      return;
   dataPlot->setMaterialComponent(GL_SPECULAR, val / 100.);
   dataPlot->updateGL();
}
void LightingDlg::setShin(int val)
{
   if (!dataPlot)
      return;
   dataPlot->setShininess( val / 100.);
   dataPlot->updateGL();
}

void LightingDlg::reset()
{
   plot->reset();
   if (dataPlot)
      dataPlot->updateGL();
}

void LightingDlg::setDistance(int val)
{
  
   plot->stick->setPos(0,0,val/100.);
   plot->updateData();
   plot->updateGL();
  
   double drad = (dataPlot->hull().maxVertex-dataPlot->hull().minVertex).length();
   drad *= val/20.;

   dataPlot->setLightShift(drad,drad,drad);
   dataPlot->updateGL();
}

void LightingDlg::assign(Qwt3D::Plot3D* pl) 
{
   if (!pl) 
      return;
   dataPlot = pl;
}

void LightingDlg::setRotation(double x, double y, double z)
{
   if (!dataPlot)
      return;
  
   setDistance(distSL->value());
   dataPlot->setLightRotation(x,y,z);
   dataPlot->updateGL();
}

#include <sba/proj.h>

namespace sba
{
  Proj::Proj(int ci, Eigen::Vector3d &q, bool stereo)
      : ndi(ci), kp(q), stereo(stereo), isValid(true) {}
      
  Proj::Proj(int ci, Eigen::Vector2d &q) 
      : ndi(ci), kp(q(0), q(1), 0), stereo(false), isValid(true) {}
  
  Proj::Proj() 
      : ndi(0), kp(0, 0, 0), isValid(false) {}

  void Proj::setJacobians(const Node &nd, const Point &pt)
  {
    if (stereo)
      setJacobiansStereo_(nd, pt);
    else
      setJacobiansMono_(nd, pt);
  }
  
  double Proj::calcErr(const Node &nd, const Point &pt)
  {
    if (stereo)
      return calcErrStereo_(nd, pt);
    else
      return calcErrMono_(nd, pt);
  }
  
  double Proj::getErrNorm()
  {
    if (stereo)
      return err.norm();
    else
      return err.start<2>().norm();
  }
  
  double Proj::getErrSquaredNorm()
  {
    if (stereo)
      return err.squaredNorm();
    else
      return err.start<2>().squaredNorm();
  }


  void Proj::setJacobiansMono_(const Node &nd, const Point &pt)
  {
    // first get the world point in camera coords
    Eigen::Matrix<double,3,1> pc = nd.w2n * pt;

    /// jacobian with respect to frame; uses dR'/dq from Node calculation
    Eigen::Matrix<double,2,6> jacc;
    
    /// jacobian with respect to point
    Eigen::Matrix<double,2,3> jacp;

    // Jacobians wrt camera parameters
    // set d(quat-x) values [ pz*dpx/dx - px*dpz/dx ] / pz^2
    double px = pc(0);
    double py = pc(1);
    double pz = pc(2);
    double ipz2 = 1.0/(pz*pz);
    if (isnan(ipz2) ) { printf("[SetJac] infinite jac\n");  *(int *)0x0 = 0; }

    double ipz2fx = ipz2*nd.Kcam(0,0); // Fx
    double ipz2fy = ipz2*nd.Kcam(1,1); // Fy
    // scale quaternion derivative to match the translational ones
    double ipz2fxq = qScale*ipz2fx;
    double ipz2fyq = qScale*ipz2fy;
    Eigen::Matrix<double,3,1> pwt;

    // check for local vars
    pwt = (pt-nd.trans).start<3>(); // transform translations, use differential rotation

    // dx
    Eigen::Matrix<double,3,1> dp = nd.dRdx * pwt; // dR'/dq * [pw - t]
    jacc(0,3) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,3) = (pz*dp(1) - py*dp(2))*ipz2fyq;
    // dy
    dp = nd.dRdy * pwt; // dR'/dq * [pw - t]
    jacc(0,4) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,4) = (pz*dp(1) - py*dp(2))*ipz2fyq;
    // dz
    dp = nd.dRdz * pwt; // dR'/dq * [pw - t]
    jacc(0,5) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,5) = (pz*dp(1) - py*dp(2))*ipz2fyq;

    // set d(t) values [ pz*dpx/dx - px*dpz/dx ] / pz^2
    dp = -nd.w2n.col(0);        // dpc / dx
    jacc(0,0) = (pz*dp(0) - px*dp(2))*ipz2fx;
    jacc(1,0) = (pz*dp(1) - py*dp(2))*ipz2fy;
    dp = -nd.w2n.col(1);        // dpc / dy
    jacc(0,1) = (pz*dp(0) - px*dp(2))*ipz2fx;
    jacc(1,1) = (pz*dp(1) - py*dp(2))*ipz2fy;
    dp = -nd.w2n.col(2);        // dpc / dz
    jacc(0,2) = (pz*dp(0) - px*dp(2))*ipz2fx;
    jacc(1,2) = (pz*dp(1) - py*dp(2))*ipz2fy;

    // Jacobians wrt point parameters
    // set d(t) values [ pz*dpx/dx - px*dpz/dx ] / pz^2
    dp = nd.w2n.col(0); // dpc / dx
    jacp(0,0) = (pz*dp(0) - px*dp(2))*ipz2fx;
    jacp(1,0) = (pz*dp(1) - py*dp(2))*ipz2fy;
    dp = nd.w2n.col(1); // dpc / dy
    jacp(0,1) = (pz*dp(0) - px*dp(2))*ipz2fx;
    jacp(1,1) = (pz*dp(1) - py*dp(2))*ipz2fy;
    dp = nd.w2n.col(2); // dpc / dz
    jacp(0,2) = (pz*dp(0) - px*dp(2))*ipz2fx;
    jacp(1,2) = (pz*dp(1) - py*dp(2))*ipz2fy;

#ifdef DEBUG
    for (int i=0; i<2; i++)
      for (int j=0; j<6; j++)
        if (isnan(jacc(i,j)) ) { printf("[SetJac] NaN in jacc(%d,%d)\n", i, j);  *(int *)0x0 = 0; }
#endif
    
    // Set Hessians + extras.
    Hpp = jacp.transpose() * jacp;
    Hcc = jacc.transpose() * jacc;
    Hpc = jacp.transpose() * jacc;
    JcTE = jacc.transpose() * err.start<2>();
    Bp = jacp.transpose() * err.start<2>();
  }

  // calculate error of a projection
  // we should do something about negative Z
  double Proj::calcErrMono_(const Node &nd, const Point &pt)
  {
    Eigen::Vector3d p1 = nd.w2i * pt; err = p1.start(2)/p1(2); 
    if (p1(2) <= 0.0) 
    {
#ifdef DEBUG
      printf("[CalcErr] negative Z! Node %d point %d\n",ndi,pti);
      if (isnan(err[0]) || isnan(err[1]) ) printf("[CalcErr] NaN!\n"); 
#endif
      err = Eigen::Vector3d(0.0,0.0,0.0);
      return 0.0;
    }
    err -= kp;
    return err.start<2>().squaredNorm(); 
  }


  void Proj::setJacobiansStereo_(const Node &nd, const Point &pt)
  {
    // first get the world point in camera coords
    Eigen::Matrix<double,3,1> pc = nd.w2n * pt;

    /// jacobian with respect to point
    Eigen::Matrix<double,3,3> jacp;
    
    /// jacobian with respect to frame; uses dR'/dq from Node calculation
    Eigen::Matrix<double,3,6> jacc;

    // Jacobians wrt camera parameters
    // set d(quat-x) values [ pz*dpx/dx - px*dpz/dx ] / pz^2
    double px = pc(0);
    double py = pc(1);
    double pz = pc(2);
    double ipz2 = 1.0/(pz*pz);
    if (isnan(ipz2) ) { printf("[SetJac] infinite jac\n");  *(int *)0x0 = 0; }

    double ipz2fx = ipz2*nd.Kcam(0,0); // Fx
    double ipz2fy = ipz2*nd.Kcam(1,1); // Fy
    double b      = nd.baseline; // stereo baseline
    // scale quaternion derivative to match the translational ones
    double ipz2fxq = qScale*ipz2fx;
    double ipz2fyq = qScale*ipz2fy;
    Eigen::Matrix<double,3,1> pwt;

    // check for local vars
    pwt = (pt-nd.trans).start(3); // transform translations, use differential rotation

    // dx
    Eigen::Matrix<double,3,1> dp = nd.dRdx * pwt; // dR'/dq * [pw - t]
    jacc(0,3) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,3) = (pz*dp(1) - py*dp(2))*ipz2fyq;
    jacc(2,3) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px
    // dy
    dp = nd.dRdy * pwt; // dR'/dq * [pw - t]
    jacc(0,4) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,4) = (pz*dp(1) - py*dp(2))*ipz2fyq;
    jacc(2,4) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px
    // dz
    dp = nd.dRdz * pwt; // dR'/dq * [pw - t]
    jacc(0,5) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,5) = (pz*dp(1) - py*dp(2))*ipz2fyq;
    jacc(2,5) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px

    // set d(t) values [ pz*dpx/dx - px*dpz/dx ] / pz^2
    dp = -nd.w2n.col(0);        // dpc / dx
    jacc(0,0) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,0) = (pz*dp(1) - py*dp(2))*ipz2fy;
    jacc(2,0) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px
    dp = -nd.w2n.col(1);        // dpc / dy
    jacc(0,1) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,1) = (pz*dp(1) - py*dp(2))*ipz2fy;
    jacc(2,1) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px
    dp = -nd.w2n.col(2);        // dpc / dz
    jacc(0,2) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacc(1,2) = (pz*dp(1) - py*dp(2))*ipz2fy;
    jacc(2,2) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px

    // Jacobians wrt point parameters
    // set d(t) values [ pz*dpx/dx - px*dpz/dx ] / pz^2
    dp = nd.w2n.col(0); // dpc / dx
    jacp(0,0) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacp(1,0) = (pz*dp(1) - py*dp(2))*ipz2fy;
    jacp(2,0) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px
    dp = nd.w2n.col(1); // dpc / dy
    jacp(0,1) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacp(1,1) = (pz*dp(1) - py*dp(2))*ipz2fy;
    jacp(2,1) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px
    dp = nd.w2n.col(2); // dpc / dz
    jacp(0,2) = (pz*dp(0) - px*dp(2))*ipz2fxq;
    jacp(1,2) = (pz*dp(1) - py*dp(2))*ipz2fy;
    jacp(2,2) = (pz*dp(0) - (px-b)*dp(2))*ipz2fxq; // right image px

#ifdef DEBUG
    for (int i=0; i<2; i++)
      for (int j=0; j<6; j++)
        if (isnan(jacc(i,j)) ) { printf("[SetJac] NaN in jacc(%d,%d)\n", i, j);  *(int *)0x0 = 0; }
#endif        

    // Set Hessians + extras.
    Hpp = jacp.transpose() * jacp;
    Hcc = jacc.transpose() * jacc;
    Hpc = jacp.transpose() * jacc;
    JcTE = jacc.transpose() * err;
    Bp = jacp.transpose() * err;
    
  }

  // calculate error of a projection
  // we should do something about negative Z
  double Proj::calcErrStereo_(const Node &nd, const Point &pt)
  { 
    Eigen::Vector3d p1 = nd.w2i * pt; 
    Eigen::Vector3d p2 = nd.w2n * pt; 
    Eigen::Vector3d pb(nd.baseline,0,0);
    double invp1 = 1.0/p1(2);
    err.start(2) = p1.start(2)*invp1;
    // right camera px
    p2 = nd.Kcam*(p2-pb);
    err(2) = p2(0)/p2(2);
    if (p1(2) <= 0.0) 
    {
#ifdef DEBUG
      printf("[CalcErr] negative Z! Node %d point %d\n",ndi,pti);
      if (isnan(err[0]) || isnan(err[1]) ) printf("[CalcErr] NaN!\n"); 
#endif
      err = Eigen::Vector3d(0.0,0.0,0.0);
      return 0.0;
    }
    err -= kp;
    return err.squaredNorm(); 
  }
  
  // Constructors for track.
  Track::Track() : point() { }
  Track::Track(Point p) : point(p) { }
      
} // sba


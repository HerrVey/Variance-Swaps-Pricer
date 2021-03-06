#include <cmath>
#include <algorithm>
#include <iostream>
#include "HestonVariancePathSimulator.h"
#include "MathFunctions.h"

HestonVariancePathSimulator::HestonVariancePathSimulator(
        const std::vector<double>& timePoints,
        const HestonModel& hestonModel):
    PathSimulator(hestonModel.getInitialVolatility(),
                  timePoints),
    hestonModel_(new HestonModel(hestonModel))
{
    preComputations();
}

HestonVariancePathSimulator::HestonVariancePathSimulator
                        (const HestonVariancePathSimulator& variancePathSimulator):
    PathSimulator(variancePathSimulator.initialValue_,variancePathSimulator.timePoints_),
    hestonModel_(new HestonModel(*(variancePathSimulator.hestonModel_)))
{

}

HestonVariancePathSimulator::~HestonVariancePathSimulator()
{
    delete hestonModel_;
}

HestonVariancePathSimulator& HestonVariancePathSimulator::operator=(
                        const HestonVariancePathSimulator& variancePathSimulator)
{
    if (this == &variancePathSimulator)
		return *this;
	else
	{
		delete hestonModel_;												
		hestonModel_ = new HestonModel(*(variancePathSimulator.hestonModel_));

        initialValue_ = variancePathSimulator.initialValue_;
        timePoints_ = variancePathSimulator.timePoints_;
	}
	return *this;
}


void HestonVariancePathSimulator::preComputations()
{
    double theta = hestonModel_->getMeanReversionLevel();
    double kappa = hestonModel_->getMeanReversionSpeed();
    double eps = hestonModel_->getVolOfVol();
    double delta;
    double expMinusKappaDelta;

    //Pre-computation of k1, k2, k3, k4 s.t. m = k1 V + k2 and s² = k3 V + k4
    // NB : we allow the time grid to be non-equidistant so that the computed quantities are time dependent.
    for(std::size_t i = 0; i < timePoints_.size()-1; i++)
    {
        delta = timePoints_[i+1] - timePoints_[i];
        expMinusKappaDelta = exp(-kappa*delta);
        k1_.push_back(expMinusKappaDelta);
        k2_.push_back(theta*(1-expMinusKappaDelta));
        k3_.push_back(eps*eps*expMinusKappaDelta*(1-expMinusKappaDelta)/kappa);
        k4_.push_back(theta*eps*eps*(1-expMinusKappaDelta)*(1-expMinusKappaDelta)/(2*kappa));
    }
}

std::vector<double> HestonVariancePathSimulator::path() const
{
    std::vector<double> path {initialValue_};
    for (std::size_t index = 0; index < timePoints_.size() - 1; ++index)
        path.push_back(nextStep(index, path[index]));

    return path;
}

HestonModel HestonVariancePathSimulator::getHestonModel() const
{
    return *hestonModel_;
}

TruncatedGaussianScheme::TruncatedGaussianScheme(const std::vector<double>& timePoints,
                                                 const HestonModel& hestonModel,
                                                 double confidenceMultiplier,
                                                 std::size_t psiGridSize,
                                                 double initialGuess):
    HestonVariancePathSimulator(timePoints,hestonModel),
    confidenceMultiplier_(confidenceMultiplier),
    psiGrid_(std::vector<double>(psiGridSize)),
    initialGuess_(initialGuess)
{
    preComputationsTG();
}

TruncatedGaussianScheme::TruncatedGaussianScheme(const TruncatedGaussianScheme& truncatedGaussianScheme):
    HestonVariancePathSimulator(truncatedGaussianScheme.timePoints_,
                                *truncatedGaussianScheme.hestonModel_),
    confidenceMultiplier_(truncatedGaussianScheme.confidenceMultiplier_),
    psiGrid_(truncatedGaussianScheme.psiGrid_),
    initialGuess_(truncatedGaussianScheme.initialGuess_),
    fmu_(truncatedGaussianScheme.fmu_), fsigma_(truncatedGaussianScheme.fsigma_)
{
    
}

TruncatedGaussianScheme* TruncatedGaussianScheme::clone() const
{
    return new TruncatedGaussianScheme(*this);
}

void TruncatedGaussianScheme::preComputationsTG()
{
    double theta = hestonModel_->getMeanReversionLevel();
    double kappa = hestonModel_->getMeanReversionSpeed();
    double eps = hestonModel_->getVolOfVol();

    //We first construct a grid for psi 
    double min = 1.0/(confidenceMultiplier_*confidenceMultiplier_);
    double max = eps*eps/(2*kappa*theta);
    psiGrid_ = MathFunctions::buildLinearSpace(min,max,psiGrid_.size());
    double r, psi, phi, Phi;
    for(std::size_t i = 0; i < psiGrid_.size(); i++)
    {
        psi = psiGrid_[i];
        //We look for r that nullifies the function h by using a Newton method
        r = MathFunctions::newtonMethod(initialGuess_,
                                        [psi](double r){return h(r,psi);},
                                        [psi](double r){return hPrime(r,psi);});
        phi = MathFunctions::normalPDF(r);
        Phi = MathFunctions::normalCDF(r);
        fmu_.push_back(r/(phi+r*Phi));
        fsigma_.push_back(pow(psi,-0.5)/(phi+r*Phi));
    }
}

double TruncatedGaussianScheme::nextStep(std::size_t currentIndex, double currentValue) const
{
    //We used the pre-computed coefficients to compute m and s²
    double m = k1_[currentIndex]*currentValue + k2_[currentIndex];
    double s2 = k3_[currentIndex]*currentValue + k4_[currentIndex];
    double psi = s2/(m*m);
    double mu, sigma;

    //If psi is close to 0, we skip the moment-fitting step
    if(psi < 1.0/(confidenceMultiplier_*confidenceMultiplier_))
    {
        mu = m;
        sigma = sqrt(s2);
    }
    else 
    {   
        //We look for i such that psiGrid_[i] <= psi < psiGrid_[i+1]
        std::size_t idxPhi = MathFunctions::binarySearch(psiGrid_,psi);
        double psi0 = psiGrid_[idxPhi], psi1 = psiGrid_[idxPhi+1];
        //Linear interpolation of fmu and fsigma using the pre-computed values
        double fmu = (fmu_[idxPhi]*(psi1-psi)+fmu_[idxPhi+1]*(psi-psi0))/(psi1-psi0);
        double fsigma = (fsigma_[idxPhi]*(psi1-psi)+fsigma_[idxPhi+1]*(psi-psi0))/(psi1-psi0);
        mu = fmu*m;
        sigma = fsigma*sqrt(s2);
    }
    double z = MathFunctions::simulateGaussianRandomVariable();
    return std::max(mu+sigma*z,0.0);
}   

double TruncatedGaussianScheme::h(double r, double psi)
{
    double phi = MathFunctions::normalPDF(r);
    double Phi = MathFunctions::normalCDF(r);

    return r*phi+Phi*(1+r*r)-(1+psi)*(phi+r*Phi)*(phi+r*Phi);
}

double TruncatedGaussianScheme::hPrime(double r, double psi)
{
    double phi = MathFunctions::normalPDF(r);
    double Phi = MathFunctions::normalCDF(r);

    return 2*phi+2*r*Phi-2*(1+psi)*Phi*(phi+r*Phi);
}


QuadraticExponentialScheme::QuadraticExponentialScheme(const std::vector<double>& timePoints,
                                                       const HestonModel& hestonModel, double psiC):
    HestonVariancePathSimulator(timePoints,hestonModel), psiC_(psiC)
{

}

QuadraticExponentialScheme::QuadraticExponentialScheme(const QuadraticExponentialScheme&
                                                       quadraticExponentialScheme):
    HestonVariancePathSimulator(quadraticExponentialScheme.timePoints_,
                                *quadraticExponentialScheme.hestonModel_),
    psiC_(quadraticExponentialScheme.psiC_)
{
    
}

QuadraticExponentialScheme* QuadraticExponentialScheme::clone() const
{
    return new QuadraticExponentialScheme(*this);
}

double QuadraticExponentialScheme::nextStep(std::size_t currentIndex, double currentValue) const{
    
    //We used the pre-computed coefficients to compute m and s²
    double m = k1_[currentIndex]*currentValue + k2_[currentIndex];
    double s2 = k3_[currentIndex]*currentValue + k4_[currentIndex];
    double psi = s2/(m*m);
    double U = MathFunctions::simulateUniformRandomVariable();

    if (psi<psiC_){
        double temp_value = 2./psi;
        double b = std::sqrt(temp_value - 1. + std::sqrt(temp_value*(temp_value-1.)));
        double a = m/(1+b*b);

        /*Since we already computed a uniform random variable, we use here
        Moho's inverse of the normal cdf instead of Box-Müller method as for TG */
        double Zv = MathFunctions::normalCDFInverse(U);
        return a*(b+Zv)*(b+Zv);
    }
    else {
        double p = (psi-1.)/(psi+1.);
        if (U<=p){
            return 0.;
        }
        else{
            double beta = (1-p)/m;
            return std::log((1-p)/(1-U))/beta;
        }
    }
}


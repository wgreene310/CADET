// =============================================================================
//  CADET - The Chromatography Analysis and Design Toolkit
//  
//  Copyright © 2008-2018: The CADET Authors
//            Please see the AUTHORS and CONTRIBUTORS file.
//  
//  All rights reserved. This program and the accompanying materials
//  are made available under the terms of the GNU Public License v3.0 (or, at
//  your option, any later version) which accompanies this distribution, and
//  is available at http://www.gnu.org/licenses/gpl.html
// =============================================================================

#include <catch.hpp>
#include "Approx.hpp"
#include "cadet/cadet.hpp"

#define CADET_LOGGING_DISABLE
#include "Logging.hpp"

#include "ColumnTests.hpp"
#include "SimHelper.hpp"
#include "ModelBuilderImpl.hpp"
#include "common/Driver.hpp"
#include "Weno.hpp"

#include "JsonTestModels.hpp"
#include "JacobianHelper.hpp"

#include <cmath>
#include <functional>
#include <cstdio>

/**
 * @brief Returns the absolute path to the test/ folder of the project
 * @details Absolute path to the test/ folder of the project without trailing slash
 * @return Absolute path to the test/ folder
 */
const char* getTestDirectory();

namespace
{
	inline RelApprox makeApprox(double val, double relTol, double absTol)
	{
		return RelApprox(val).epsilon(relTol).margin(absTol);
	}

	/**
	 * @brief Fills the state vector with a given function
	 * @details The function @p f uses the current index to assign a value.
	 * @param [out] y Filled state vector
	 * @param [in] f Function for computing the content of the state vector
	 * @param [in] numDofs Size of the state vector
	 */
	inline void fillState(double* y, std::function<double(unsigned int)> f, unsigned int numDofs)
	{
		for (unsigned int i = 0; i < numDofs; ++i)
			y[i] = f(i);
	}

	/**
	 * @brief Creates a runnable column model with given WENO order
	 * @details Creates a column model and configures it using the given IParameterProvider @p jpp.
	 * @param [in] uoType Unit operation type
	 * @param [in] mb ModelBuilder
	 * @param [in] jpp Configuration of the model
	 * @param [in] wenoOrder WENO order
	 * @return Runnable column model
	 */
	inline cadet::IUnitOperation* createAndConfigureUnit(const std::string& uoType, cadet::IModelBuilder& mb, cadet::JsonParameterProvider& jpp, int wenoOrder)
	{
		// Create a unit
		cadet::IModel* const iUnit = mb.createUnitOperation(uoType, 0);
		REQUIRE(nullptr != iUnit);

		cadet::IUnitOperation* const unit = reinterpret_cast<cadet::IUnitOperation*>(iUnit);

		// Set WENO order
		cadet::test::column::setWenoOrder(jpp, wenoOrder);
		// Configure
		cadet::ModelBuilder& temp = *reinterpret_cast<cadet::ModelBuilder*>(&mb);
		REQUIRE(unit->configureModelDiscretization(jpp, temp));
		REQUIRE(unit->configure(jpp));

		// Do some checks
		const unsigned int nComp = jpp.getInt("NCOMP");
		REQUIRE(unit->numComponents() == nComp);

		return unit;
	}
}

namespace cadet
{

namespace test
{

namespace column
{

	/**
	 * @brief Reads reference chromatograms from a test data file
	 * @details The file format is as follows:
	 *          Number of data points (uint32)
	 *          Time points (array of doubles)
	 *          Chromatogram for dynamic binding (array of doubles)
	 *          Chromatogram for quasi-stationary binding (array of doubles)
	 */
	class ReferenceDataReader
	{
	public:
		ReferenceDataReader(const char* fileName) : _f(nullptr)
		{
			_f = std::fopen(fileName, "rb");
			std::fread(&_numElements, 4, 1, _f);
		}

		~ReferenceDataReader()
		{
			std::fclose(_f);
		}

		std::vector<double> time()
		{
			std::vector<double> v(_numElements, 0.0);
			std::fseek(_f, 4, SEEK_SET);
			std::fread(v.data(), 8, _numElements, _f);
			return v;
		}

		std::vector<double> analyticDynamic()
		{
			std::vector<double> v(_numElements, 0.0);
			std::fseek(_f, 4 + _numElements * 8, SEEK_SET);
			std::fread(v.data(), 8, _numElements, _f);
			return v;
		}

		std::vector<double> analyticQuasiStationary()
		{
			std::vector<double> v(_numElements, 0.0);
			std::fseek(_f, 4 + 2 * _numElements * 8, SEEK_SET);
			std::fread(v.data(), 8, _numElements, _f);
			return v;
		}

	private:
		std::FILE* _f;
		uint32_t _numElements;
	};

	void setNumAxialCells(cadet::JsonParameterProvider& jpp, unsigned int nCol)
	{
		int level = 0;

		if (jpp.exists("model"))
		{
			jpp.pushScope("model");
			++level;
		}
		if (jpp.exists("unit_000"))
		{
			jpp.pushScope("unit_000");
			++level;
		}

		jpp.pushScope("discretization");

		jpp.set("NCOL", static_cast<int>(nCol));

		jpp.popScope();
	
		for (int l = 0; l < level; ++l)
			jpp.popScope();
	}

	void setWenoOrder(cadet::JsonParameterProvider& jpp, int order)
	{
		int level = 0;

		if (jpp.exists("model"))
		{
			jpp.pushScope("model");
			++level;
		}
		if (jpp.exists("unit_000"))
		{
			jpp.pushScope("unit_000");
			++level;
		}

		jpp.pushScope("discretization");
		jpp.pushScope("weno");

		jpp.set("WENO_ORDER", order);

		jpp.popScope();
		jpp.popScope();
	
		for (int l = 0; l < level; ++l)
			jpp.popScope();
	}

	void reverseFlow(cadet::JsonParameterProvider& jpp)
	{
		int level = 0;

		if (jpp.exists("model"))
		{
			jpp.pushScope("model");
			++level;
		}
		if (jpp.exists("unit_000"))
		{
			jpp.pushScope("unit_000");
			++level;
		}

		jpp.set("VELOCITY", -jpp.getDouble("VELOCITY"));

		for (int l = 0; l < level; ++l)
			jpp.popScope();
	}

	void setBindingMode(cadet::JsonParameterProvider& jpp, bool isKinetic)
	{
		int level = 0;

		if (jpp.exists("model"))
		{
			jpp.pushScope("model");
			++level;
		}
		if (jpp.exists("unit_000"))
		{
			jpp.pushScope("unit_000");
			++level;
		}

		jpp.pushScope("adsorption");
		jpp.set("IS_KINETIC", isKinetic);
		jpp.popScope();

		for (int l = 0; l < level; ++l)
			jpp.popScope();
	}

	void setCrossSectionArea(cadet::JsonParameterProvider& jpp, bool useTotalPorosity, int dir)
	{
		int level = 0;

		if (jpp.exists("model"))
		{
			jpp.pushScope("model");
			++level;
		}
		if (jpp.exists("unit_000"))
		{
			jpp.pushScope("unit_000");
			++level;
		}

		const double vel = jpp.getDouble("VELOCITY");
		double por = 0.0;
		if (useTotalPorosity && (jpp.exists("TOTAL_POROSITY")))
			por = jpp.getDouble("TOTAL_POROSITY");
		else
			por = jpp.getDouble("COL_POROSITY");

		// Assume a volumetric flow rate of 1.0 m^3/s
		jpp.set("CROSS_SECTION_AREA", 1.0 / (vel * por));

		if (dir == 0)
			jpp.remove("VELOCITY");
		else
		{
			if (dir > 0)
				jpp.set("VELOCITY", 1.0);
			else
				jpp.set("VELOCITY", -1.0);
		}

		for (int l = 0; l < level; ++l)
			jpp.popScope();
	}


	void testWenoForwardBackward(const char* uoType, int wenoOrder, double absTol, double relTol)
	{
		SECTION("Forward vs backward flow (WENO=" + std::to_string(wenoOrder) + ")")
		{
			// Use Load-Wash-Elution test case
			cadet::JsonParameterProvider jpp = createLWE(uoType);
			setWenoOrder(jpp, wenoOrder);

			// Forward flow
			cadet::Driver drvFwd;
			drvFwd.configure(jpp);
			drvFwd.run();

			// Backward flow
			reverseFlow(jpp);
			cadet::Driver drvBwd;
			drvBwd.configure(jpp);
			drvBwd.run();

			cadet::InternalStorageUnitOpRecorder const* const fwdData = drvFwd.solution()->unitOperation(0);
			cadet::InternalStorageUnitOpRecorder const* const bwdData = drvBwd.solution()->unitOperation(0);

			double const* fwdInlet = fwdData->inlet();
			double const* fwdOutlet = fwdData->outlet();
			double const* bwdInlet = bwdData->inlet();
			double const* bwdOutlet = bwdData->outlet();

			const unsigned int nComp = fwdData->numComponents();
			for (unsigned int i = 0; i < fwdData->numDataPoints() * nComp; ++i, ++fwdInlet, ++fwdOutlet, ++bwdInlet, ++bwdOutlet)
			{
				// Forward flow inlet = backward flow outlet
				CAPTURE(i);
				CHECK((*fwdInlet) == makeApprox(*bwdOutlet, relTol, absTol));

				// Forward flow outlet = backward flow inlet
				CAPTURE(i);
				CHECK((*fwdOutlet) == makeApprox(*bwdInlet, relTol, absTol));
			}
		}
	}

	void testAnalyticBenchmark(const char* uoType, const char* refFileRelPath, bool forwardFlow, bool dynamicBinding, unsigned int nCol, double absTol, double relTol)
	{
		const std::string fwdStr = (forwardFlow ? "forward" : "backward");
		SECTION("Analytic " + fwdStr + " flow with " + (dynamicBinding ? "dynamic" : "quasi-stationary") + " binding")
		{
			// Setup simulation
			cadet::JsonParameterProvider jpp = createLinearBenchmark(dynamicBinding, false, uoType);
			setNumAxialCells(jpp, nCol);
			if (!forwardFlow)
				reverseFlow(jpp);

			// Run simulation
			cadet::Driver drv;
			drv.configure(jpp);
			drv.run();

			// Read reference data from test file
			const std::string refFile = std::string(getTestDirectory()) + std::string(refFileRelPath);
			ReferenceDataReader rd(refFile.c_str());
			const std::vector<double> time = rd.time();
			const std::vector<double> ref = (dynamicBinding ? rd.analyticDynamic() : rd.analyticQuasiStationary());

			// Get data from simulation
			cadet::InternalStorageUnitOpRecorder const* const simData = drv.solution()->unitOperation(0);
			double const* outlet = (forwardFlow ? simData->outlet() : simData->inlet());

			// Compare
			for (unsigned int i = 0; i < simData->numDataPoints(); ++i, ++outlet)
			{
				// Note that the simulation only saves the chromatogram at multiples of 2 (i.e., 0s, 2s, 4s, ...)
				// whereas the reference solution is given at every second (0s, 1s, 2s, 3s, ...)
				// Thus, we only take the even indices of the reference array
				CAPTURE(time[2 * i]);
				CHECK((*outlet) == makeApprox(ref[2 * i], relTol, absTol));
			}
		}
	}

	void testAnalyticNonBindingBenchmark(const char* uoType, const char* refFileRelPath, bool forwardFlow, unsigned int nCol, double absTol, double relTol)
	{
		const std::string fwdStr = (forwardFlow ? "forward" : "backward");
		SECTION("Analytic " + fwdStr + " flow")
		{
			// Setup simulation
			cadet::JsonParameterProvider jpp = createLinearBenchmark(true, true, uoType);
			setNumAxialCells(jpp, nCol);
			if (!forwardFlow)
				reverseFlow(jpp);

			// Run simulation
			cadet::Driver drv;
			drv.configure(jpp);
			drv.run();

			// Read reference data from test file
			const std::string refFile = std::string(getTestDirectory()) + std::string(refFileRelPath);
			ReferenceDataReader rd(refFile.c_str());
			const std::vector<double> time = rd.time();
			const std::vector<double> ref = rd.analyticDynamic();

			// Get data from simulation
			cadet::InternalStorageUnitOpRecorder const* const simData = drv.solution()->unitOperation(0);
			double const* outlet = (forwardFlow ? simData->outlet() : simData->inlet());

			// Compare
			for (unsigned int i = 0; i < simData->numDataPoints(); ++i, ++outlet)
			{
				CAPTURE(time[i]);
				CHECK((*outlet) == makeApprox(ref[i], relTol, absTol));
			}
		}
	}

	void testJacobianWenoForwardBackward(const std::string& uoType, int wenoOrder)
	{
		cadet::IModelBuilder* const mb = cadet::createModelBuilder();
		REQUIRE(nullptr != mb);

		SECTION("Forward vs backward flow Jacobian (WENO=" + std::to_string(wenoOrder) + ")")
		{
			// Use some test case parameters
			cadet::JsonParameterProvider jpp = createGRMwithLinear();
			const unsigned int nComp = jpp.getInt("NCOMP");

			cadet::IUnitOperation* const unitAna = createAndConfigureUnit(uoType, *mb, jpp, wenoOrder);
			cadet::IUnitOperation* const unitAD = createAndConfigureUnit(uoType, *mb, jpp, wenoOrder);

			// Enable AD
			cadet::ad::setDirections(cadet::ad::getMaxDirections());
			unitAD->useAnalyticJacobian(false);

			cadet::active* adRes = new cadet::active[unitAD->numDofs()];
			cadet::active* adY = new cadet::active[unitAD->numDofs()];

			unitAD->prepareADvectors(adRes, adY, 0);

			// Setup matrices
			unitAna->notifyDiscontinuousSectionTransition(0.0, 0u, nullptr, nullptr, 0u);
			unitAD->notifyDiscontinuousSectionTransition(0.0, 0u, adRes, adY, 0u);

			// Obtain memory for state, Jacobian multiply direction, Jacobian column
			std::vector<double> y(unitAD->numDofs(), 0.0);
			std::vector<double> jacDir(unitAD->numDofs(), 0.0);
			std::vector<double> jacCol1(unitAD->numDofs(), 0.0);
			std::vector<double> jacCol2(unitAD->numDofs(), 0.0);

			// Fill state vector with some values
			fillState(y.data(), [](unsigned int idx) { return std::abs(std::sin(idx * 0.13)) + 1e-4; }, unitAna->numDofs());
//			fillState(y.data(), [](unsigned int idx) { return 1.0; }, unitAna->numDofs());

			// Obtain number of column cells
			jpp.pushScope("discretization");
			const unsigned int nCol = jpp.getInt("NCOL");
			REQUIRE(nCol == 15u);
			jpp.popScope();

			SECTION("Forward then backward flow (nonzero state)")
			{
				// Compute state Jacobian
				unitAna->residualWithJacobian(0.0, 0u, 1.0, y.data(), nullptr, jacDir.data(), nullptr, nullptr, 0u);
				unitAD->residualWithJacobian(0.0, 0u, 1.0, y.data(), nullptr, jacDir.data(), adRes, adY, 0u);
				std::fill(jacDir.begin(), jacDir.end(), 0.0);

				// Compare Jacobians
				cadet::test::checkJacobianPatternFD(unitAna, unitAD, y.data(), nullptr, jacDir.data(), jacCol1.data(), jacCol2.data());
				cadet::test::checkJacobianPatternFD(unitAna, unitAna, y.data(), nullptr, jacDir.data(), jacCol1.data(), jacCol2.data());
				cadet::test::compareJacobian(unitAna, unitAD, nullptr, nullptr, jacDir.data(), jacCol1.data(), jacCol2.data());
//				cadet::test::compareJacobianFD(unitAna, unitAD, y.data(), nullptr, jacDir.data(), jacCol1.data(), jacCol2.data());

				// Reverse flow
				const bool paramSet = unitAna->setParameter(cadet::makeParamId(cadet::hashString("VELOCITY"), 0, cadet::CompIndep, cadet::BoundPhaseIndep, cadet::ReactionIndep, cadet::SectionIndep), -jpp.getDouble("VELOCITY"));
				REQUIRE(paramSet);
				// Reverse flow
				const bool paramSet2 = unitAD->setParameter(cadet::makeParamId(cadet::hashString("VELOCITY"), 0, cadet::CompIndep, cadet::BoundPhaseIndep, cadet::ReactionIndep, cadet::SectionIndep), -jpp.getDouble("VELOCITY"));
				REQUIRE(paramSet2);

				// Setup
				unitAna->notifyDiscontinuousSectionTransition(0.0, 0u, nullptr, nullptr, 0u);
				unitAD->notifyDiscontinuousSectionTransition(0.0, 0u, adRes, adY, 0u);

				// Compute state Jacobian
				unitAna->residualWithJacobian(0.0, 0u, 1.0, y.data(), nullptr, jacDir.data(), nullptr, nullptr, 0u);
				unitAD->residualWithJacobian(0.0, 0u, 1.0, y.data(), nullptr, jacDir.data(), adRes, adY, 0u);
				std::fill(jacDir.begin(), jacDir.end(), 0.0);

				// Compare Jacobians
				cadet::test::checkJacobianPatternFD(unitAna, unitAD, y.data(), nullptr, jacDir.data(), jacCol1.data(), jacCol2.data());
				cadet::test::checkJacobianPatternFD(unitAna, unitAna, y.data(), nullptr, jacDir.data(), jacCol1.data(), jacCol2.data());
//				cadet::test::compareJacobianFD(unitAD, unitAna, y.data(), jacDir.data(), nullptr, jacCol1.data(), jacCol2.data());
//				cadet::test::compareJacobianFD(unitAna, unitAD, y.data(), jacDir.data(), nullptr, jacCol1.data(), jacCol2.data());
				cadet::test::compareJacobian(unitAna, unitAD, nullptr, nullptr, jacDir.data(), jacCol1.data(), jacCol2.data());
			}

			delete[] adRes;
			delete[] adY;
			mb->destroyUnitOperation(unitAna);
			mb->destroyUnitOperation(unitAD);
		}
		destroyModelBuilder(mb);
	}

	void testTimeDerivativeJacobianFD(const std::string& uoType, double h, double absTol, double relTol)
	{
		cadet::IModelBuilder* const mb = cadet::createModelBuilder();
		REQUIRE(nullptr != mb);

		// Use some test case parameters
		cadet::JsonParameterProvider jpp = createGRMwithLinear();
		const unsigned int nComp = jpp.getInt("NCOMP");

		cadet::IUnitOperation* const unit = createAndConfigureUnit(uoType, *mb, jpp, cadet::Weno::maxOrder());

		// Setup matrices
		unit->notifyDiscontinuousSectionTransition(0.0, 0u, nullptr, nullptr, 0u);

		// Obtain memory for state, Jacobian multiply direction, Jacobian column
		const unsigned int nDof = unit->numDofs();
		std::vector<double> y(nDof, 0.0);
		std::vector<double> yDot(nDof, 0.0);
		std::vector<double> jacDir(nDof, 0.0);
		std::vector<double> jacCol1(nDof, 0.0);
		std::vector<double> jacCol2(nDof, 0.0);

		// Fill state vectors with some values
		fillState(y.data(), [=](unsigned int idx) { return std::abs(std::sin(idx * 0.13)) + 1e-4; }, nDof);
		fillState(yDot.data(), [=](unsigned int idx) { return std::abs(std::sin((idx + nDof) * 0.13)) + 1e-4; }, nDof);

		// Compare Jacobians
		cadet::test::compareTimeDerivativeJacobianFD(unit, unit, y.data(), yDot.data(), jacDir.data(), jacCol1.data(), jacCol2.data(), h, absTol, relTol);

		mb->destroyUnitOperation(unit);
		destroyModelBuilder(mb);
	}

	void testFwdSensJacobians(const std::string& uoType, double h, double absTol, double relTol)
	{
		cadet::IModelBuilder* const mb = cadet::createModelBuilder();
		REQUIRE(nullptr != mb);

		// Use some test case parameters
		cadet::JsonParameterProvider jpp = createGRMwithLinear();
		const unsigned int nComp = jpp.getInt("NCOMP");

		for (int bindMode = 0; bindMode < 2; ++bindMode)
		{
			const bool isKinetic = bindMode;
			SECTION(isKinetic ? "Kinetic binding" : "Quasi-stationary binding")
			{
				cadet::test::column::setBindingMode(jpp, isKinetic);
				cadet::IUnitOperation* const unit = createAndConfigureUnit(uoType, *mb, jpp, cadet::Weno::maxOrder());

				// Enable AD
				cadet::ad::setDirections(cadet::ad::getMaxDirections());
				cadet::active* adRes = new cadet::active[unit->numDofs()];
				unit->prepareADvectors(adRes, nullptr, 0);

				// Add dispersion parameter sensitivity
				REQUIRE(unit->setSensitiveParameter(makeParamId(hashString("COL_DISPERSION"), 0, CompIndep, BoundPhaseIndep, ReactionIndep, SectionIndep), 0, 1.0));

				// Setup matrices
				unit->notifyDiscontinuousSectionTransition(0.0, 0u, adRes, nullptr, 0u);

				// Obtain memory for state, Jacobian multiply direction, Jacobian column
				const unsigned int nDof = unit->numDofs();
				const std::vector<double> zeros(nDof, 0.0);
				const std::vector<double> ones(nDof, 1.0);
				std::vector<double> y(nDof, 0.0);
				std::vector<double> yDot(nDof, 0.0);
				std::vector<double> jacDir(nDof, 0.0);
				std::vector<double> jacCol1(nDof, 0.0);
				std::vector<double> jacCol2(nDof, 0.0);
				std::vector<double> temp1(nDof, 0.0);
				std::vector<double> temp2(nDof, 0.0);
				std::vector<double> temp3(nDof, 0.0);

				std::vector<const double*> yS(1, zeros.data());
				std::vector<const double*> ySdot(1, zeros.data());
				std::vector<double*> resS(1, nullptr);

				// Fill state vector with some values
				fillState(y.data(), [](unsigned int idx) { return std::abs(std::sin(idx * 0.13)) + 1e-4; }, nDof);
				fillState(yDot.data(), [=](unsigned int idx) { return std::abs(std::sin((idx + nDof) * 0.13)) + 1e-4; }, nDof);

				// Calculate Jacobian
				unit->residualWithJacobian(0.0, 0u, 1.0, y.data(), yDot.data(), jacDir.data(), adRes, nullptr, 0u);

				// Calculate parameter derivative
				unit->residualSensFwdAdOnly(0.0, 0u, 1.0, y.data(), yDot.data(), adRes);

				// Check state Jacobian
				cadet::test::compareJacobianFD(
					[&](double const* lDir, double* res) -> void {
						yS[0] = lDir;
						resS[0] = res;
						unit->residualSensFwdCombine(0.0, 0u, 1.0, y.data(), yDot.data(), yS, ySdot, resS, adRes, temp1.data(), temp2.data(), temp3.data());
					}, 
					[&](double const* lDir, double* res) -> void { unit->multiplyWithJacobian(0.0, 0u, 1.0, y.data(), yDot.data(), lDir, 1.0, 0.0, res); }, 
					zeros.data(), jacDir.data(), jacCol1.data(), jacCol2.data(), nDof, h, absTol, relTol);

				// Reset evaluation point
				yS[0] = zeros.data();
				ySdot[0] = zeros.data();

				// Check time derivative Jacobian
				cadet::test::compareJacobianFD(
					[&](double const* lDir, double* res) -> void {
						ySdot[0] = lDir;
						resS[0] = res;
						unit->residualSensFwdCombine(0.0, 0u, 1.0, y.data(), yDot.data(), yS, ySdot, resS, adRes, temp1.data(), temp2.data(), temp3.data());
					}, 
					[&](double const* lDir, double* res) -> void { unit->multiplyWithDerivativeJacobian(0.0, 0u, 1.0, y.data(), yDot.data(), lDir, res); }, 
					zeros.data(), jacDir.data(), jacCol1.data(), jacCol2.data(), nDof, h, absTol, relTol);

				delete[] adRes;
				mb->destroyUnitOperation(unit);
			}
		}

		destroyModelBuilder(mb);
	}

	void testFwdSensSolutionFD(const std::string& uoType, bool disableSensErrorTest, double const* fdStepSize, double const* absTols, double const* relTols, double const* passRates)
	{
		const std::vector<cadet::ParameterId> params = {
			cadet::makeParamId("COL_DISPERSION", 0, cadet::CompIndep, cadet::BoundPhaseIndep, cadet::ReactionIndep, cadet::SectionIndep),
			cadet::makeParamId("CONST_COEFF", 1, 0, cadet::BoundPhaseIndep, cadet::ReactionIndep, 0),
			cadet::makeParamId("SMA_KA", 0, 1, 0, cadet::ReactionIndep, cadet::SectionIndep),
			cadet::makeParamId("CONNECTION", cadet::UnitOpIndep, cadet::CompIndep, 1, 0, 0),
		};
		const std::vector<const char*> paramNames = {"COL_DISPERSION", "CONST_COEFF", "SMA_KA", "CONNECTION"};
		const double absTolSens[] = {1e-12, 1e-6, 1e-6, 1e-6};

		for (int bindMode = 0; bindMode < 2; ++bindMode)
		{
			const bool isKinetic = bindMode;
			for (std::size_t n = 0; n < params.size(); ++n)
			{
				SECTION("Parameter " + std::string(paramNames[n]) + (isKinetic ? " Kinetic binding" : " Quasi-stationary binding"))
				{
					const double absTol = absTols[n];
					const double relTol = relTols[n];
					const double passRate = passRates[n];
					const cadet::ParameterId& curParam = params[n];
					const double h = fdStepSize[n];

					// Setup simulation including forward sensitivities
					cadet::JsonParameterProvider jppAna = createLWE(uoType);
					cadet::test::column::setBindingMode(jppAna, isKinetic);
					cadet::test::column::setCrossSectionArea(jppAna, uoType == "LUMPED_RATE_MODEL_WITHOUT_PORES", 0);
					cadet::test::addSensitivity(jppAna, paramNames[n], curParam, absTolSens[n]);
					cadet::test::returnSensitivities(jppAna, 0);
					if (disableSensErrorTest)
						cadet::test::disableSensitivityErrorTest(jppAna);

					// Run simulation
					cadet::Driver drv;
					drv.configure(jppAna);
					drv.run();

					// Setup FD simulation
					cadet::JsonParameterProvider jppFD = createLWE(uoType);
					cadet::test::column::setBindingMode(jppFD, isKinetic);
					cadet::test::column::setCrossSectionArea(jppFD, uoType == "LUMPED_RATE_MODEL_WITHOUT_PORES", 0);

					// Configure FD simulation
					cadet::Driver drvLeft;
					drvLeft.configure(jppFD);
					
					// Extract parameter values
					const double baseVal = drvLeft.simulator()->model()->getParameterDouble(curParam);
					REQUIRE(!std::isnan(baseVal));

					// Run left FD point
					drvLeft.simulator()->setParameterValue(curParam, baseVal * (1.0 - h));
					drvLeft.run();

					// Configure and run right FD simulation
					cadet::Driver drvRight;
					drvRight.configure(jppFD);
					drvRight.simulator()->setParameterValue(curParam, baseVal * (1.0 + h));
					drvRight.run();

					// Get data from simulation
					cadet::InternalStorageUnitOpRecorder const* const simData = drv.solution()->unitOperation(0);
					const unsigned int nComp = simData->numComponents();
					double const* time = drv.solution()->time();
					double const* outlet = simData->sensOutlet(0);
					double const* outletL = drvLeft.solution()->unitOperation(0)->outlet();
					double const* outletR = drvRight.solution()->unitOperation(0)->outlet();

					// Compare
					const double actStepSize = 2.0 * h * baseVal;
					unsigned int numPassed = 0;
					for (unsigned int i = 0; i < simData->numDataPoints(); ++i, ++outlet, ++outletL, ++outletR)
					{
						const double cmpVal = *outlet;
						const double fdVal = ((*outletR) - (*outletL)) / actStepSize;
						const unsigned int comp = i % nComp;
						const unsigned int timeIdx = i / nComp;

						INFO("Time " << (*time) << " Component " << comp << " time point idx " << timeIdx);
						CHECK(fdVal == makeApprox(*outlet, relTol, absTol));

						const bool relativeOK = std::abs(*outlet - fdVal) <= relTol * std::abs(*outlet);
						if (relativeOK)
							++numPassed;

						if (i % nComp == 0)
							++time;
					}

					const double ratio = static_cast<double>(numPassed) / static_cast<double>(simData->numDataPoints());
					CAPTURE(ratio);
					CHECK(ratio >= passRate);
				}
			}
		}
	}

	void testFwdSensSolutionForwardBackward(const std::string& uoType, double const* absTols, double const* relTols, double const* passRates)
	{
		const std::vector<cadet::ParameterId> params = {
			cadet::makeParamId("COL_DISPERSION", 0, cadet::CompIndep, cadet::BoundPhaseIndep, cadet::ReactionIndep, cadet::SectionIndep),
			cadet::makeParamId("CONST_COEFF", 1, 0, cadet::BoundPhaseIndep, cadet::ReactionIndep, 0),
			cadet::makeParamId("SMA_KA", 0, 1, 0, cadet::ReactionIndep, cadet::SectionIndep),
			cadet::makeParamId("CONNECTION", cadet::UnitOpIndep, cadet::CompIndep, 1, 0, 0),
		};
		const std::vector<const char*> paramNames = {"COL_DISPERSION", "CONST_COEFF", "SMA_KA", "CONNECTION"};
		const double absTolSens[] = {1e-12, 1e-6, 1e-6, 1e-6};

		for (int bindMode = 0; bindMode < 2; ++bindMode)
		{
			const bool isKinetic = bindMode;
			for (std::size_t n = 0; n < params.size(); ++n)
			{
				SECTION("Parameter " + std::string(paramNames[n]) + (isKinetic ? " Kinetic binding" : " Quasi-stationary binding"))
				{
					const double absTol = absTols[n];
					const double relTol = relTols[n];
					const double passRate = passRates[n];
					const cadet::ParameterId& curParam = params[n];

					// Setup simulation including forward sensitivities
					cadet::JsonParameterProvider jpp = createLWE(uoType);
					cadet::test::column::setBindingMode(jpp, isKinetic);
					cadet::test::column::setCrossSectionArea(jpp, uoType == "LUMPED_RATE_MODEL_WITHOUT_PORES", 1);
					cadet::test::addSensitivity(jpp, paramNames[n], curParam, absTolSens[n]);
					cadet::test::returnSensitivities(jpp, 0, true);
					cadet::test::disableSensitivityErrorTest(jpp);

					// Run simulation
					cadet::Driver drvFwd;
					drvFwd.configure(jpp);
					drvFwd.run();

					// Run simulation with reversed flow
					reverseFlow(jpp);
					cadet::Driver drvBwd;
					drvBwd.configure(jpp);
					drvBwd.run();

					// Get data from simulation
					cadet::InternalStorageUnitOpRecorder const* const fwdData = drvFwd.solution()->unitOperation(0);
					cadet::InternalStorageUnitOpRecorder const* const bwdData = drvBwd.solution()->unitOperation(0);

					const unsigned int nComp = fwdData->numComponents();
					double const* time = drvFwd.solution()->time();
					double const* fwdOutlet = fwdData->sensOutlet(0);
					double const* bwdInlet = bwdData->sensInlet(0);

					// Compare
					unsigned int numPassed = 0;
					for (unsigned int i = 0; i < fwdData->numDataPoints(); ++i, ++fwdOutlet, ++bwdInlet)
					{
						const unsigned int comp = i % nComp;
						const unsigned int timeIdx = i / nComp;

						INFO("Time " << (*time) << " Component " << comp << " time point idx " << timeIdx);
						CHECK(*bwdInlet == makeApprox(*fwdOutlet, relTol, absTol));

						const bool relativeOK = std::abs(*bwdInlet - *fwdOutlet) <= relTol * std::abs(*fwdOutlet);
						if (relativeOK)
							++numPassed;

						if (i % nComp == 0)
							++time;
					}

					const double ratio = static_cast<double>(numPassed) / static_cast<double>(fwdData->numDataPoints());
					CAPTURE(ratio);
					CHECK(ratio >= passRate);
				}
			}
		}
	}

} // namespace column
} // namespace test
} // namespace cadet

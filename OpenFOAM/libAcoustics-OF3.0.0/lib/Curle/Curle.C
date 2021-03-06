/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "Curle.H"
#include "volFields.H"
#include "dictionary.H"
#include "Time.H"
#include "wordReList.H"

#include "RASModel.H"
#include "LESModel.H"

#include "basicThermo.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //
namespace Foam
{

    defineTypeNameAndDebug(Curle, 0);

}
// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

Foam::tmp<Foam::scalarField> Foam::Curle::normalStress(const word& patchName) const
{

    const fvMesh& mesh = refCast<const fvMesh>(obr_);
    const volScalarField& p = mesh.lookupObject<volScalarField>(pName_);
    
    label patchId = mesh.boundary().findPatchID(patchName);
    
    scalarField pPatch = p.boundaryField()[patchId];
    
    if (p.dimensions() == dimPressure)
    {
	//return tmp<scalarField>
	//(
	//    new scalarField(pPatch)
	//);
    }
    else
    {
	if (rhoRef_ < 0) //density in volScalarField
	{
	    scalarField pRho = mesh.lookupObject<volScalarField>(rhoName_).
				boundaryField()[patchId];
	    pPatch *= pRho;
	}
	else //density is constant
	{
	    pPatch *= rhoRef_;
	}
    }

    return tmp<scalarField>
    (
	new scalarField(pPatch)
    );
}

// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::Curle::Curle
(
    const word& name,
    const objectRegistry& obr,
    const dictionary& dict,
    const bool loadFromFiles
)
:
    name_(name),
    obr_(obr),
    active_(true),
    probeFreq_(1),
    log_(false),
    patchNames_(0, word::null),
    timeStart_(-1.0),
    timeEnd_(-1.0),
    pName_(word::null),
    c0_(300.0),
    dRef_(-1.0),
    observers_(0),
    rhoName_(word::null),
    rhoRef_(1.0),
    c_(vector::zero),
    CurleFilePtr_(NULL),
    FOldPtr_(NULL),
    FOldOldPtr_(NULL),
    probeI_(0)
{
    // Check if the available mesh is an fvMesh otherise deactivate
    if (!isA<fvMesh>(obr_))
    {
        active_ = false;
        WarningIn
        (
            "Foam::Curle::Curle"
            "("
                "const word&, "
                "const objectRegistry&, "
                "const dictionary&, "
                "const bool"
            ")"
        )   << "No fvMesh available, deactivating."
            << endl;
    }

    read(dict);
}

// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::Curle::~Curle()
{}

// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void Foam::Curle::read(const dictionary& dict)
{
    if (!active_)
    {
	return;
    }

    log_ = dict.lookupOrDefault<Switch>("log", false);
    
    if (!log_)
    {
	Info << "Direct logging to stdio disabled" << endl
	    << " to enable, please insert string:" << endl
	    << "log\t\t true;" << endl
	    << "in dictionary" << endl;
    }
    
    dict.lookup("probeFrequency") >> probeFreq_;

    //const fvMesh& mesh = refCast<const fvMesh>(obr_);

    dict.lookup("patchNames") >> patchNames_;
    
    dict.lookup("timeStart") >> timeStart_;
    
    dict.lookup("timeEnd") >> timeEnd_;
    
    dict.lookup("c0") >> c0_;
    
    dict.lookup("dRef") >> dRef_;

    dict.lookup("pName") >> pName_;
    
    dict.lookup("rhoName") >> rhoName_;
    
    dict.lookup("rhoRef") >> rhoRef_;

    //read observers
    {
	const dictionary& obsDict = dict.subDict("observers");
	wordList obsNames = obsDict.toc();
	forAll (obsNames, obsI)
	{
	    word oname = obsNames[obsI];
	    vector opos (vector::zero);
	    obsDict.subDict(oname).lookup("position") >> opos;
	    scalar pref = 1.0e-5;
	    obsDict.subDict(oname).lookup("pRef") >> pref;
	    label fftFreq = 1024;
	    obsDict.subDict(oname).lookup("fftFreq") >> fftFreq;
	    
	    observers_.append
	    (
		SoundObserver
		(
		    oname,
		    opos,
		    pref,
		    fftFreq
		)
	    );
	}
    }
    
    calcDistances();   
}

void Foam::Curle::correct()
{
    const fvMesh& mesh = refCast<const fvMesh>(obr_);
    
    //sign '-' needed to calculate force, which exerts fluid by solid
    vector F	(0.0, 0.0, 0.0);
    vector dFdT (0.0, 0.0, 0.0);
    scalar deltaT = mesh.time().deltaT().value();
    
    forAll(patchNames_, iPatch)
    {
	word patchName = patchNames_[iPatch];
	label patchId = mesh.boundary().findPatchID(patchName);
	
	scalarField pp = normalStress(patchName);
	F -= gSum (pp * mesh.Sf().boundaryField()[patchId]);
    }
    
    if (Pstream::master() || !Pstream::parRun())
    {
	//calculate dFdT and store old values
	
	if (FOldPtr_.empty())
	{
	    FOldPtr_.set
	    (
		new vector(F)
	    );
	}
	else
	{
	    if (FOldOldPtr_.empty())
	    {
		//first order scheme
		dFdT = (F - FOldPtr_()) / deltaT;
		
		FOldOldPtr_.set
		(
		    FOldPtr_.ptr()
		);
		
		FOldPtr_.reset
		(
		    new vector(F)
		);
	    }
	    else
	    {
		//second order scheme (BDF)
		dFdT = (3.0*F - 4.0*FOldPtr_() + FOldOldPtr_()) / 2.0 / deltaT;
		
		FOldOldPtr_.reset
		(
		    FOldPtr_.ptr()
		);
		
		FOldPtr_.reset
		(
		    new vector(F)
		);
	    }
	}
	
	scalar coeff1 = 1. / 4. / Foam::constant::mathematical::pi / c0_;
	
	forAll (observers_, iObs)
	{
	    SoundObserver& obs = observers_[iObs];
	    //Vector from observer to center
	    vector l = obs.position() - c_;
	    //Calculate distance
	    scalar r = mag(l);
	    //Calculate ObservedAcousticPressure
	    scalar oap = l & (dFdT + c0_ * F / r) * coeff1 / r / r;
	    if (dRef_ > 0.0)
	    {
		oap /= dRef_;
	    }
	    obs.apressure(oap); //appends new calculated acoustic pressure
	    
	    //noiseFFT addition
	    obs.atime(mesh.time().value());
	}
	
    }
}

void Foam::Curle::makeFile()
{
    fileName CurleDir;

    if (Pstream::master() && Pstream::parRun())
    {
	CurleDir = obr_.time().rootPath() + "/" + obr_.time().caseName().path()  + "/acousticData";
	mkDir(CurleDir);
    }
    else if (!Pstream::parRun())
    {
	CurleDir = obr_.time().rootPath() + "/" + obr_.time().caseName() + "/acousticData";
	mkDir(CurleDir);
    }
    else
    {
    }
    // File update
    if (Pstream::master() || !Pstream::parRun())
    {
	// Create the Curle file if not already created
	if (CurleFilePtr_.empty())
	{
	    // Open new file at start up
	    CurleFilePtr_.reset
	    (
		new OFstream
		(
		    CurleDir + "/" + (name_ + "-time.dat")
		)
	    );
	    
	    writeFileHeader();
	}
    }
}


void Foam::Curle::writeFileHeader()
{
    if (CurleFilePtr_.valid())
    {
        CurleFilePtr_()
            << "Time" << " ";
	
        forAll(observers_, iObserver)
        {
	    CurleFilePtr_() << observers_[iObserver].name() << "_pFluct ";
        }

        CurleFilePtr_()<< endl;
    }
}

void Foam::Curle::calcDistances()
{
    if (!active_)
    {
	return;
    }

    const fvMesh& mesh = refCast<const fvMesh>(obr_);
    
    label patchId = mesh.boundary().findPatchID(patchNames_[0]);
    
    if (patchId < 0)
    {
	List<word> patchNames(0);
	forAll (mesh.boundary(), iPatch)
	{
	    patchNames.append(mesh.boundary()[iPatch].name());
	}
	FatalErrorIn
	(
	    "Foam::Curle::calcDistances()"
	)   << "Can\'t find path "
	<< patchNames_[0]
	<< "Valid patches are : " << nl
	<< patchNames
	<< exit(FatalError);
    }
    
    vectorField ci = mesh.boundary()[patchId].Cf();
    scalar ni = scalar(ci.size());
    reduce (ni, sumOp<scalar>());
    
    c_ = gSum(ci) / ni;
}

void Foam::Curle::writeFft()
{
    fileName CurleDir;

    if (Pstream::master() && Pstream::parRun())
    {
	CurleDir = obr_.time().rootPath() + "/" + obr_.time().caseName().path()  + "/acousticData";
    }
    else if (!Pstream::parRun())
    {
	CurleDir = obr_.time().rootPath() + "/" + obr_.time().caseName() + "/acousticData";
    }
    
    if (Pstream::master() || !Pstream::parRun())
    {
	const fvMesh& mesh = refCast<const fvMesh>(obr_);
	//Save timestep for FFT transformation in tau
	scalar tau = probeFreq_*mesh.time().deltaT().value();
        Info << "Executing fft for obs: " << name_ << endl;
	forAll(observers_, iObserver)
	{
	    SoundObserver& obs = observers_[iObserver];
	    
	    autoPtr<List<List<scalar> > > obsFftPtr (obs.fft(tau));
	    
	    List<List<scalar> >& obsFft = obsFftPtr();
	    
  	    if (obsFft[0].size() > 0)
	    {

		fileName fftFile = CurleDir + "/fft-" + name_ + "-" + obs.name() + ".dat";
		
		OFstream fftStream (fftFile);
		fftStream << "Freq p\' spl" << endl;
		
		forAll(obsFft[0], k)
		{
		    fftStream << obsFft[0][k] << " " << obsFft[1][k] << " " << obsFft[2][k] << endl;
		}
		
		fftStream.flush();
	    }
	}
    }
}

void Foam::Curle::execute()
{
    if (!active_)
    {
	return;
    }

    // Create the Curle file if not already created
    makeFile();

    scalar cTime = obr_.time().value();
    
    probeI_++;
    
    if ( mag(probeI_ % probeFreq_) > VSMALL  )
    {
	return;
    }
    else
    {
	if (log_)
	{
	    Info << "Starting acoustics probe" << endl;
	}
	probeI_ = 0.0;
    }
    
    if ( (cTime < timeStart_) || (cTime > timeEnd_))
    {
	return;
    }
    
    correct();
    
    if (Pstream::master() || !Pstream::parRun())
    {
	// time history output
	CurleFilePtr_() << (cTime - timeStart_) << " ";
	
	forAll(observers_, iObserver)
	{
	    const SoundObserver& obs = observers_[iObserver];
	    CurleFilePtr_() << obs.apressure() << " ";
	}
	
	CurleFilePtr_() << endl;
	
	//fft output
	writeFft();
	
	//output to stdio
	if (log_)
	{
	    Info << "Curle acoustic pressure" << endl;
	    forAll(observers_, iObserver)
	    {
		const SoundObserver& obs = observers_[iObserver];
		Info << "Observer: " << obs.name() << " p\' = " << obs.apressure() << endl;
	    }
	    Info << endl;
	}
    }
}

void Foam::Curle::end()
{
    // Do nothing - only valid on execute
}

void Foam::Curle::timeSet()
{
    // Do nothing - only valid on write
}

void Foam::Curle::write()
{
    // Do nothing - only valid on execute
}

// ************************************************************************* //

/*
 soundScore -- Sound Spectogram anaylize and scoring tool
 Copyright (C) 2014 copyright Shen Yiming <sym@shader.cn>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

// Standard include files
#include <iostream>
#include <getopt.h>
#include <string.h>
#include <math.h>
#include <assert.h>

// libsndfile addon include file
#include <sndfile.h>

// Opencv addon include file
#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui.hpp>

// Portaudio 2.0 include file
#include <portaudio.h>

// kissFFT addon include file
#include "kiss_fft.h"
#include "kiss_fftr.h"

// local includes
#include "common.h"
#include "soundView.h"

// libsndfile can handle more than 6 channels but we'll restrict it to 6. */
#define MAX_CHANNELS 2
// Mat height & width
#define WIDTH 512
#define HEIGHT 256
// Define use visual
#define USE_VISUAL true
#define VIS_TOPFREQ 256
using namespace std;

void
paExitWithError(PaError err)
{
    Pa_Terminate();
    cerr << "[Error] An error occured while using the portaudio stream" << endl ;
    cerr << "Error number   : " << err << endl;
    cerr << "Error message  : " << Pa_GetErrorText( err ) << endl;
    exit(err);
}; /* paExitWithError */

soundView::Params::Params()
{
    // Default setting is using microphone input
    inputDevice = USE_MIC;
    outputDevice = Pa_GetDefaultOutputDevice();
    sampleRate = 44100;
}; /* soundView::Params::Params() */

soundView::soundView(const soundView::Params &parameters) :
    stream(0), volume(1), floor_db(0), max_db(200), col(0),
    spectogram(cv::Size(WIDTH,HEIGHT),CV_8UC3), params(parameters)
{

    //
    // Initialization of FFT
    //
    fftcfg = kiss_fftr_alloc( 2 * BUFFER_LEN, 0, NULL, NULL);
    if(fftcfg == NULL){
        cerr << "[Error] Not enough memory!" << endl;
        exit(-1) ;
    }

    // memory allocation of sound data
    inputData = new float[BUFFER_LEN];
    if (inputData == NULL) {
        cerr << "[Error] Not enough memory!" << endl;
        exit(-1);
    }

    //
    // Initialization I/O
    //
    if (params.inputDevice == USE_FILE)
    {
        if (!init_file()) exit(-1);
    }
    else
    {
        if (!init_mic()) exit(-1);
    }

    for(size_t i=0; i< 2*BUFFER_LEN; i++) in[i] = 0;

    //spectogram= cv::Mat(cv::Size(WIDTH, HEIGHT),CV_8UC3);

}; /* soundView::soundView() */

void
soundView::init()
{
    //
    // Initializing portaudio output device
    //
    PaError err = Pa_Initialize();
    if (err != paNoError)
        paExitWithError(err);
    err = Pa_GetDeviceCount();
    if (err < 0)
        paExitWithError(err);
    std::cout << "[Info] Available audio devices : " << err << endl;
}

void
soundView::close()
{
    PaError err = Pa_Terminate();
    if (err != paNoError)
        paExitWithError(err);
}; /* soundView::close() */

bool
soundView::start()
{
    PaError err;

    // First need to check if the stream is occupied
    err = Pa_IsStreamActive( stream );
    if (err == 1) {
        cerr << "[Warning] Audio stream is busy, trying to end previous session..."<< endl;
        err = Pa_CloseStream( stream );
        if(err != paNoError){
            cerr << "[Error] Failed to end audio session, quitting..." << endl;
            paExitWithError(err);
        }
    }
    // else if(err < 0)
        // paExitWithError(err);

    if (params.inputDevice == USE_MIC)
    {
        err = Pa_OpenStream( &stream, &inputParams, NULL, /* no output in record mode */
                    params.sampleRate, BUFFER_LEN, paClipOff, RecordCallback, this);
        if(err!=paNoError)
            paExitWithError( err );
        err = Pa_StartStream( stream );
        if(err!=paNoError)
            paExitWithError( err );
        cout << ">>>> Now recording , please speak to the microphone. <<<<" << endl;
    }
    else
    {
        sndHandle.seek(0, SEEK_SET); // seek to the start of sound file.

        unsigned int chn = sndHandle.channels();

        if(params.outputDevice != paNoDevice)
        {
            err = Pa_OpenStream(&stream, NULL, &outputParams, params.sampleRate,
                                BUFFER_LEN * chn, paClipOff, playCallback, this);
            if(err!=paNoError)
                paExitWithError( err );
            err = Pa_StartStream( stream );
            if(err!=paNoError)
                paExitWithError( err );
            cout << ">>>> Playing file : " << params.inputFilename << " <<<<" << endl;
        }
        else
        {
            // If no output needed, read file and generate spectogram directly
            while(true) {
                //unsigned int chn = sndHandle.channels();
                sf_count_t readCount;
                if(chn > 1)
                {
                    // if channel > 1, mix down to mono audio data first
                    float *multiChannelData = new float[BUFFER_LEN * chn];
                    readCount = sndHandle.read(multiChannelData, BUFFER_LEN * chn);

                    for (size_t i=0; i<BUFFER_LEN; i++) {
                        float mix = 0;
                        for(size_t j = 0; j < chn; j++)
                            mix += multiChannelData[ i * chn + j ];
                        *inputData++ = mix/chn;
                    }
                    readCount = readCount/chn;
                }
                else
                    readCount = sndHandle.read(inputData, BUFFER_LEN);

                drawBuffer(inputData);

                // end loop while reaching end of file
                if((readCount) < BUFFER_LEN) break;

            }
        }
    }
    return true;
} /* soundView::start() */

bool
soundView::stop()
{
    PaError err;
    err = Pa_CloseStream( stream );
    if(err != paNoError)
        paExitWithError(err);
    //cout << "[Info] Audio stream closed for this session." << endl;
    return true;
} /* soundView:stop() */

void
soundView::setLevels(const float _volume, const float _max_db, const float _floor_db)
{
    volume = _volume;
    max_db = _max_db;
    floor_db = _floor_db;
} /* soundView::setLevels */

bool
soundView::isPlayback()
{
    if(params.inputDevice == USE_FILE)
        return params.outputDevice == paNoDevice ? false : true;
    else
        return true;
} /* soundView::isPlayback */

bool
soundView::isRecord()
{
    return params.inputDevice == USE_MIC ? true : false;
} /* soundView::isRecord */

int
soundView::isPlaying()
{
    PaError err = Pa_IsStreamActive(stream);
    if(err < 0) paExitWithError(err);
    return err;
} /* soundView::isPlaying */

cv::Mat
soundView::Spectogram()
{
    return spectogram;
}

bool
soundView::init_file()
{
    //
    // Initialization of libsndfile setup
    //
    sndHandle = SndfileHandle(params.inputFilename);

    if (! sndHandle.rawHandle()) {
        /* Open failed so print an error message. */
        std::cerr << "[Error] Not able to open input file : " << params.inputFilename << endl ;
        /* Print the error message from libsndfile. */
        puts ( sndHandle.strError() );
        return false;
    }else {
        cout << "Opening sound file : " << params.inputFilename << endl;
        cout << "Frames             : " << sndHandle.frames() << endl;
        cout << "Sample rate        : " << sndHandle.samplerate() << endl;
        cout << "Channels           : " << sndHandle.channels() << endl;
    }

    if (sndHandle.channels() > MAX_CHANNELS) {
        printf ("[Error] Not able to process more than %d channels\n", MAX_CHANNELS) ;
        return false;
    }

    // Initialize the portaudio inputParams
    //
    outputParams.device = params.outputDevice;
    if(outputParams.device != params.outputDevice) {
        std::cerr << "[Error] Failed to open audio output device: " << outputParams.device << endl;
        return false;
    }else {
        cout << "Using device       : " << outputParams.device << endl;
        if(outputParams.device != paNoDevice){

            outputParams.channelCount = sndHandle.channels();
            outputParams.sampleFormat = paFloat32;
            outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultLowInputLatency;
            outputParams.hostApiSpecificStreamInfo = NULL;
        }
    }

    return true;
} /* soundView::init_file() */

bool
soundView::init_mic()
{
    // Initialize portaudio inputParams
    inputParams.channelCount = 1;
    inputParams.device = Pa_GetDefaultInputDevice();
    if(inputParams.device == paNoDevice){
        std::cerr << "[Error] Failed to open audio input device: " << inputParams.device << endl;
        return false;
    }
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = NULL;

    return true;
} /* soundView::init_mic() */


//
// Private member function implementation
//

int
soundView::RecordCallback(const void *input,
                          void *output,
                          unsigned long framePerBuffer,
                          const PaStreamCallbackTimeInfo *timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData)
{
    return ((soundView*)userData)->RecordCallbackImpl(input, output,
                        framePerBuffer, timeInfo, statusFlags);
} /* soundView::RecordCallback */

int
soundView::RecordCallbackImpl(  const void *input,
                    void *output,
                    unsigned long framePerBuffer,
                    const PaStreamCallbackTimeInfo *timeInfo,
                    PaStreamCallbackFlags statusFlags)
{
    const float* rptr = (const float*)input;
    size_t i;

    (void) output;
    (void) timeInfo;
    (void) statusFlags;

    // fill output stream with libsndfile data
    for(i=0; i<framePerBuffer; i++ ){
        inputData[i] = rptr[i] * volume; // multiply by volume setting
        //inputData++, rptr++;
    }
    drawBuffer(inputData);
    return paContinue;
} /* soundView:RecordCallbackImpl */

int
soundView::playCallback(const void *input,
                          void *output,
                          unsigned long framePerBuffer,
                          const PaStreamCallbackTimeInfo *timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData)
{
    return ((soundView*)userData)->playCallbackImpl(input, output,
                        framePerBuffer, timeInfo, statusFlags);
} /* soundView::playCallback */

int
soundView::playCallbackImpl(const void *input,
                    void *output,
                    unsigned long framePerBuffer,
                    const PaStreamCallbackTimeInfo *timeInfo,
                    PaStreamCallbackFlags statusFlags)
{
    float* wptr = (float*)output;
    size_t i;
    sf_count_t readCount;

    (void) input ;
    (void) timeInfo;
    (void) statusFlags;

    // If it's dual channel audio, double the read buffer
    // And downsample to mono data
    unsigned int chn = sndHandle.channels();
    if (chn > 1)
    {
        float* steroData = new float[framePerBuffer];
        readCount = sndHandle.read(steroData, framePerBuffer);
        for (i=0; i<framePerBuffer/chn; i++)
        {
            float mix = 0;
            for(size_t j = 0; j < chn; j++)
            {
                mix += steroData[ i * chn + j ];
                *wptr++ = steroData[i * chn +j ] * volume;
            }
            inputData[i] = mix/chn;
        }
        //readCount = readCount / chn;

    }
    else{
        readCount = sndHandle.read(inputData, framePerBuffer);
        for(i=0; i<framePerBuffer; i++)
            *wptr++ = inputData[i] * volume;
    }
    drawBuffer(inputData);


    // Read sndfile in, check if the file reach the end?
    // If so, return to the start of sndfile (loop)
    // Or, finish playing with paComplete

    if( readCount < (sf_count_t)framePerBuffer) {
        // sndHandle.seek(0, SEEK_SET);
        //cout << "[Info] Playback finished, Press 'r' to replay." << endl;
        return paComplete;
    }

    return paContinue;
} /* soundView::playbackImpl */

void
soundView::drawBuffer(const void* input)
{

    const float* data = (const float*)input;
    size_t i;
    //
    // Do time domain windowing and FFT convertion
    //
    float mag [ BUFFER_LEN ] , interp_mag [ HEIGHT ];
    kiss_fft_scalar in_win[ 2 * BUFFER_LEN];

    for(i=0; i<BUFFER_LEN; i++){
        in[i] = in[ BUFFER_LEN + i ];
        in[ BUFFER_LEN + i ] = data[i];
    }

    apply_window(in_win, in , 2 * BUFFER_LEN);
    kiss_fftr(fftcfg, in_win, out);

    // 0Hz set to 0
    mag[0] = 0;
    float max_mag = 0;
    for(int i = 1; i < BUFFER_LEN; i++){
        mag[i] = std::sqrtf(out[i].i * out[i].i +
                            out[i].r * out[i].r);
        // Convert to dB range 20log10(v1/v2)
        mag[i] = 20 * log10(mag[i]);
        // Convert to RGB space
        mag[i] = linestep(mag[i], floor_db, max_db) * 255;
        max_mag = std::max(max_mag, mag[i]);
    }
    if (max_mag > 0 && col < WIDTH) {
        interp_spec(interp_mag, HEIGHT, mag, VIS_TOPFREQ); // Draw sound spectogram

        cv::line(spectogram, cv::Point2i(col,0), cv::Point2i(col,HEIGHT), cv::Scalar(0,0,0));
        cv::line(spectogram, cv::Point2i(col+1,0), cv::Point2i(col+1,HEIGHT), cv::Scalar(0,0,255));
        for(int row = 0; row < HEIGHT; row++){
            spectogram.at<cv::Vec3b>(row, col)
                = cv::Vec3b(    interp_mag[row],
                                interp_mag[row],
                                interp_mag[row]);
        }
        // col = (col+1) % WIDTH;
        col++;
    }
} /* soundView::drawBuffer */

void
soundView::drawRawBuffer(const void* input)
{

    const float* data = (const float*)input;
    size_t i;
    //
    // Do time domain windowing and FFT convertion
    //
    float mag [ BUFFER_LEN ];

    for(i=0; i<BUFFER_LEN; i++){
        in[i] = in[ BUFFER_LEN + i ];
        in[ BUFFER_LEN + i ] = data[i];
    }

    kiss_fftr(fftcfg, in, out);

    // 0Hz set to 0
    mag[0] = 0;
    float max_mag = 0;
    for(int i = 1; i < BUFFER_LEN; i++){
        mag[i] = std::sqrtf(out[i].i * out[i].i +
                            out[i].r * out[i].r);
        // Convert to dB range 20log10(v1/v2)
        mag[i] = 20 * log10(mag[i]);
        // Convert to RGB space
        mag[i] = linestep(mag[i], floor_db, max_db) * 255;
        max_mag = std::max(max_mag, mag[i]);
    }
    if (max_mag > 0) {

        cv::line(spectogram, cv::Point2i(col,0), cv::Point2i(col,HEIGHT), cv::Scalar(0,0,0));
        cv::line(spectogram, cv::Point2i(col+1,0), cv::Point2i(col+1,HEIGHT), cv::Scalar(0,0,255));
        for(int row = 0; row < HEIGHT; row++){
            spectogram.at<cv::Vec3b>(row, col)
                = cv::Vec3b(    mag[row],
                                mag[row],
                                mag[row]);
        }
        col = (col+1) % WIDTH;
    }
} /* soundView::drawBuffer */





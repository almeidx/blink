#pragma once

#include <QString>

namespace blink {

// Where an S3-compatible bucket lives and how to sign requests for it.
struct S3Config {
    QString endpoint;        // https://s3.us-east-1.amazonaws.com, https://<account>.r2.cloudflarestorage.com, ...
    QString region;          // us-east-1; "auto" for R2
    QString bucket;
    QString accessKey;
    QString secretKey;
    QString path = QStringLiteral("{random:12}.{extension}"); // where uploads go in the bucket; placeholders: pathpattern.h
    QString publicUrl;       // base of the links handed out, e.g. https://i.example.com/; empty: the bucket's own URL
    bool pathStyle = false;  // https://endpoint/bucket/key instead of https://bucket.endpoint/key
    bool publicRead = false; // sends x-amz-acl: public-read

    bool isComplete() const; // endpoint, region, bucket and both keys are set
};

enum class Quality { Low, Medium, High };
enum class EncoderPreference { Auto, Nvenc, Vaapi, Software };

struct Settings {
    bool startAtLogin = true;
    bool notifications = true;

    // Screenshots
    bool captureCursor = false;
    bool windowDecorations = true; // active window captures include the title bar and borders
    bool windowShadow = false;
    bool magnifier = true; // region selection shows a zoomed view of the pixels around the pointer
    bool saveScreenshots = false;
    QString screenshotFolder;
    QString screenshotSubfolder = QStringLiteral("{year}-{month}"); // inside screenshotFolder; empty for none

    // Recordings, always saved
    QString recordingFolder;
    QString recordingSubfolder = QStringLiteral("{year}-{month}"); // likewise
    int fps = 60;
    Quality quality = Quality::High;
    EncoderPreference encoder = EncoderPreference::Auto;
    bool recordCursor = true;
    bool systemAudio = true;
    bool microphone = false;

    // Uploads
    bool uploadScreenshots = false;
    bool uploadRecordings = false;
    bool copyUrl = true; // the link replaces the capture on the clipboard once the upload is done
    S3Config s3;

    static Settings load();
    // Also restricts the config file to its owner, since it holds the S3 secret key.
    void save() const;
};

} // namespace blink

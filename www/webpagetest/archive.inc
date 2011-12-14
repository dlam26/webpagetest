<?php
/**
* Archive the given test if it hasn't already been archived
* For now this will just zip and move to a location on disk
* but will eventually integrate with the S3 archiving
* 
* @param mixed $id
*/
function ArchiveTest($id) {
    global $settings;
    global $api_keys;
    $ret = false;
    if (isset($settings['archive_dir']) || isset($settings['archive_s3_server'])) {
        $testPath = realpath('./' . GetTestPath($id));
        $testInfo = json_decode(gz_file_get_contents("$testPath/testinfo.json"), true);
        $completed = $testInfo['completed'];
        if (!$completed) {
            $test = parse_ini_file("$testPath/testinfo.ini", true);
            $completed = isset($test['test']['completeTime']);
        }
        if ($completed || $testInfo['batch']) {
            if( isset($testInfo['archived']) && $testInfo['archived'] ) {
                if (isset($settings['archive_dir'])) {
                    if (VerifyArchive($id))
                        $ret = true;
                    else {
                        $zipFile = GetArchiveFile($id, true);
                        unlink($zipFile);
                        $testInfo['archived'] = false;
                        gz_file_put_contents("$testPath/testinfo.json", json_encode($testInfo));
                    }
                } else
                    $ret = true;
            }
            
            if (!$ret) {
                $zipFile = "./tmp/$id.zip";
                // zip up the contents
                if (is_dir($testPath) === true) {
                    $count = 0;
                    $zip = new ZipArchive();
                    if ($zip->open($zipFile, ZIPARCHIVE::CREATE) === true) {
                        // add the files (skip the ones we don't want to archive long-term)
                        $files = scandir($testPath);
                        foreach ($files as $file) {
                            $filePath = "$testPath/$file";
                            if (is_file($filePath) && 
                                strpos($file, '_doc.jpg') === false &&
                                strpos($file, '_render.jpg') === false ) {
                                $count++;
                                $zip->addFile($filePath, $file);
                            }
                        }
                        
                        // if it is a video test, save the median first view video
                        if ($count && $testInfo['video']) {
                            $run = $testInfo['medianRun'];
                            if (!$run) {
                                require_once('page_data.inc');
                                $pageData = loadAllPageData($testPath);
                                if( $pageData )
                                    $run = GetMedianRun($pageData, 0);
                            }

                            if ($run) {
                                $relativeDir = "video_$run";
                                $videoDir = "$testPath/$relativeDir";
                                $files = scandir($videoDir);
                                if ($files) {
                                    $zip->addEmptyDir($relativeDir);
                                    foreach ($files as $file) {
                                        $filePath = "$videoDir/$file";
                                        if( is_file($filePath) )
                                            $zip->addFile($filePath, "$relativeDir/$file");
                                    }
                                }
                            }
                        }
                        
                        $zip->close();
                        
                        // move the archive to it's final destination
                        if ($count && is_file($zipFile)) {
                            if (isset($settings['archive_dir'])) {
                                $dest = GetArchiveFile($id, true);
                                if (rename($zipFile, $dest)) {
                                    if (VerifyArchive($id))
                                        $ret = true;
                                }
                            } elseif (isset($settings['archive_s3_server']) && isset($settings['archive_s3_key']) 
                                        && isset($settings['archive_s3_secret']) && isset($settings['archive_s3_bucket'])) {
                                // post the file to a S3-style bucket (just supporting Internet archive right now)
                                require_once('./lib/S3.php');
                                $s3 = new S3(trim($settings['archive_s3_key']), trim($settings['archive_s3_secret']), false, trim($settings['archive_s3_server']));
                                $separator = strrpos($id, '_');
                                if ($separator !== false) {
                                    $bucket = $settings['archive_s3_bucket'];
                                    $file = "$id.zip";
                                    $metaHeaders = array();
                                    $requestHeaders = array();
                                    if (trim($settings['archive_s3_server']) == 's3.us.archive.org') {
                                        // special-case Internet Archive storage
                                        $bucket = $settings['archive_s3_bucket'] . '_' . substr($id, 0, $separator);
                                        $file = substr($id, $separator + 1);
                                        $requestHeaders = array('x-archive-queue-derive' => '0',
                                                                'x-archive-meta-collection' => 'httparchive',
                                                                'x-archive-auto-make-bucket' => '1');
                                    }
                                    if ($s3->putObject($s3->inputFile($zipFile, false), $bucket, "$file.zip", S3::ACL_PUBLIC_READ, $metaHeaders, $requestHeaders))
                                        $ret = true;
                                }
                            }
                        } 
                        // make sure we don't leave a file hanging around
                        if (is_file($zipFile))
                            unlink($zipFile);
                        if ($ret) {
                            $testInfo['archived'] = true;
                            gz_file_put_contents("$testPath/testinfo.json", json_encode($testInfo));
                        }
                    }
                }
            }
        }
    }
    return $ret;
}

/**
* Restore the given test from the archive if it is archived
* 
* @param mixed $id
*/
function RestoreArchive($id) {
    global $settings;
    $ret = false;
    $testPath = './' . GetTestPath($id);
    if (isset($settings['archive_dir']) || isset($settings['archive_s3_url'])) {
        mkdir( $testPath, 0777, true );
        $testPath = realpath($testPath);
        $zipfile = "./tmp/$id.zip";
        if (isset($settings['archive_dir'])) {
            $source = GetArchiveFile($id);
            copy($source, $zipfile);
        } elseif (isset($settings['archive_s3_url'])) {
            $separator = strrpos($id, '_');
            if ($separator !== false) {
                $bucket = $settings['archive_s3_bucket'];
                $file = "$id.zip";
                if (trim($settings['archive_s3_server']) == 's3.us.archive.org') {
                    // special-case Internet Archive storage
                    $bucket = $settings['archive_s3_bucket'] . '_' . substr($id, 0, $separator);
                    $file = substr($id, $separator + 1);
                }
                $url = trim($settings['archive_s3_url']) . "$bucket/$file.zip";
                $data = file_get_contents($url);
                if ($data) {
                    file_put_contents($zipfile, $data);
                    unset($data);
                }
            }
        }
        $zip = new ZipArchive;
        if ($zip->open($zipfile) === TRUE) {
            if (defined('RESTORE_DATA_ONLY')) {
                for( $i = 0; $i < $zip->numFiles; $i++ ) {
                    $file = $zip->getNameIndex($i);
                    if( strpos($file, '.jpg') === false && 
                        strpos($file, '.png') == false &&
                        strpos($file, 'pagespeed.') == false &&
                        strpos($file, 'progress.') == false &&
                        strpos($file, 'report.') == false &&
                        strpos($file, '/') == false )
                        file_put_contents("$testPath/$file", $zip->getFromIndex($i));
                }
            } else {
                $zip->extractTo($testPath);
            }
            
            $zip->close();

            $testInfo = json_decode(gz_file_get_contents("$testPath/testinfo.json"), true);
            $testInfo['archived'] = true;
            $testInfo['restored'] = true;
            if( defined('RESTORE_DATA_ONLY') )
                $testInfo['restoredDataOnly'] = true;
            else
                $testInfo['restoredDataOnly'] = false;
            gz_file_put_contents("$testPath/testinfo.json", json_encode($testInfo));
            
            $ret = true;
        }    
        if (is_file($zipfile))
            unlink($zipfile);
    }
    else
        $ret = true;
        
    return $ret;
}

/**
* Verify the archive for the given test (deep verification)
* 
* @param mixed $id
*/
function VerifyArchive($id) {
    $valid = true;
    global $settings;
    
    // for now, only support verifying local archives
    if (isset($settings['archive_dir'])) {
        $valid = false;
        $testPath = realpath('./' . GetTestPath($id));
        $archive = GetArchiveFile($id);
        if (is_dir($testPath) && is_file($archive)) {
            $zip = new ZipArchive;
            if ($zip->open($archive) === TRUE) {
                // check for some basic files
                if ($zip->locateName('testinfo.ini') !== false &&
                    ($zip->locateName('testinfo.json') !== false || $zip->locateName('testinfo.json.gz') !== false)) {
                    $valid = true;
                    // now loop through the original directory and make sure all of the data files are present in the archive
                    $files = scandir($testPath);
                    foreach ($files as $file) {
                        if (strpos($file, '.txt') !== false && strpos($file, '_status.txt') === false) {
                            $index = $zip->locateName($file);
                            if ($index === false) {
                                logMsg("$id - Missing $file ($archive)");
                                $valid = false;
                                break;
                            } else {
                                $info = $zip->statIndex($index);
                                if (!$info['size']) {
                                    logMsg("$id - Invalid file size for $file ($archive)");
                                    $valid = false;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    logMsg("$id - Missing key files ($archive)");
                }
                $zip->close();
            } else {
                logMsg("$id - Zip file failed to open ($archive)");
            }
        } else {
            logMsg("$id - Zip file missing ($archive)");
        }
    }
    
    return $valid;
}

/**
* Generate the file name for the given archive file
* 
* @param mixed $id
* @param mixed $create_directory
*/
function GetArchiveFile($id, $create_directory = false)
{
    global $settings;
    $file = null;
    if (isset($settings['archive_dir']) && strlen($id)) {
        $testPath = GetTestPath($id);
        if (strlen($testPath)) {
            $file = $settings['archive_dir'] . $testPath . '.zip';
            if ($create_directory) {
                $dir = dirname($file);
                mkdir($dir, 0777, true);
            }
        }
    }
    return $file;
}

?>
/*  NOCTE Slicer home page.
 *
 *  The contract with the C++ side is unchanged (ADR-003 keeps the page, not the protocol):
 *
 *    JS -> C++   SendWXMessage(JSON.stringify({sequence_id, command, data}))  (../include/globalapi.js)
 *                which forwards to window.wx.postMessage(). The commands this page sends are
 *                handled in GUI_App::handle_web_request (GUI_App.cpp): get_recent_projects,
 *                homepage_newproject, homepage_openproject, homepage_open_recentfile,
 *                homepage_delete_recentfile, homepage_delete_all_recentfile,
 *                homepage_explore_recentfile, get_web_shortcut.
 *
 *    C++ -> JS   WebViewPanel::SendRecentList (WebViewDialog.cpp:435) runs
 *                window.postMessage({...}), which the assignment at the bottom of this file
 *                routes into HandleStudio(pVal). WebViewPanel::SetLoginPanelVisibility
 *                (WebViewDialog.cpp:431) calls SetLoginPanelVisibility() by name.
 *
 *  What is gone with the accounts (ADR-003 section 1): the Orca Cloud and Bambu Cloud rows and
 *  their menus, the network-plug-in tip, the model mall / staff picks and the OrcaCloud shortcut.
 *  The commands that fed them are simply never sent; HandleStudio ignores anything it does not
 *  know, so a stray message from an older build is harmless rather than a JS error.
 */

var RightBtnFilePath = '';
var MousePosX = 0;
var MousePosY = 0;
var sImages = {};

function OnInit()
{
	TranslatePage();
	SendMsg_GetRecentFile();
}

/*------Messages from C++------*/

function HandleStudio( pVal )
{
	let strCmd = pVal['command'];

	if (strCmd == "get_recent_projects") {
		ShowRecentFileList(pVal["response"]);
	} else if (strCmd == "studio_clickmenu") {
		GotoMenu(pVal["data"]["menu"]);
	}
	/* Everything else — the cloud, plug-in and marketplace commands — has no panel to update. */
}

/* Called by name from WebViewPanel::SetLoginPanelVisibility. There is no login panel any more,
   so this is a no-op; it must stay defined or that RunScript() raises a JS error. */
function SetLoginPanelVisibility( visible )
{
}

function GotoMenu( strMenu )
{
	let MenuList = $(".BtnItem");
	let nAll = MenuList.length;

	for (let n = 0; n < nAll; n++)
	{
		let OneBtn = MenuList[n];

		if ($(OneBtn).attr("menu") == strMenu)
		{
			$(".BtnItem").removeClass("BtnItemSelected");
			$(OneBtn).addClass("BtnItemSelected");

			$("div[board]").hide();
			$("div[board='" + strMenu + "']").show();
		}
	}
}

/*------Recent files------*/

function ShowRecentFileList( pList )
{
	let nTotal = pList.length;
	let strHtml = '';

	for (let n = 0; n < nTotal; n++)
	{
		let OneFile = pList[n];

		let sPath = OneFile['path'];
		let sImg  = OneFile['image'] || sImages[sPath];
		let sTime = OneFile['time'];
		let sName = OneFile['project_name'];
		sImages[sPath] = sImg;

		strHtml += '<div class="FileItem" fpath="' + sPath + '">' +
			'<a class="FileTip" title="' + sPath + '"></a>' +
			'<div class="FileImg"><img src="' + sImg + '" onerror="this.onerror=null;this.src=\'img/d.png\';" alt="" /></div>' +
			'<div class="FileName TextS1">' + sName + '</div>' +
			'<div class="FileDate">' + sTime + '</div>' +
			'</div>';
	}

	$("#FileList").html(strHtml);

	Set_RecentFile_MouseRightBtn_Event();
	UpdateRecentClearBtnDisplay();
}

function UpdateRecentClearBtnDisplay()
{
	let nFile = $(".FileItem").length;

	if (nFile > 0) {
		$("#RecentClearAllBtn").show();
		$("#EmptyState").hide();
	} else {
		$("#RecentClearAllBtn").hide();
		$("#EmptyState").show();
	}
}

function Set_RecentFile_MouseRightBtn_Event()
{
	$(".FileItem").mousedown(
		function(e)
		{
			RightBtnFilePath = $(this).attr('fpath');

			if (e.which == 3) {
				ShowRecnetFileContextMenu();
			} else if (e.which == 1) {
				OnOpenRecentFile( encodeURI(RightBtnFilePath) );
			}
		});

	$(document).bind("contextmenu", function(e) {
		return false;
	});

	$(document).mousemove( function(e) {
		MousePosX = e.pageX;
		MousePosY = e.pageY;
	} );

	$(document).click( function() {
		var e = e || window.event;
		var elem = e.target || e.srcElement;
		while (elem) {
			if (elem.id && elem.id == 'recnet_context_menu') {
				return;
			}
			elem = elem.parentNode;
		}

		$("#recnet_context_menu").hide();
	} );
}

function ShowRecnetFileContextMenu()
{
	$("#recnet_context_menu").offset({top: 10000, left: -10000});
	$('#recnet_context_menu').show();

	let ContextMenuWidth  = $('#recnet_context_menu').width();
	let ContextMenuHeight = $('#recnet_context_menu').height();

	let DocumentWidth  = $(document).width();
	let DocumentHeight = $(document).height();

	let RealX = MousePosX;
	let RealY = MousePosY;

	if ( MousePosX + ContextMenuWidth + 24 > DocumentWidth )
		RealX = DocumentWidth - ContextMenuWidth - 24;
	if ( MousePosY + ContextMenuHeight + 24 > DocumentHeight )
		RealY = DocumentHeight - ContextMenuHeight - 24;

	$("#recnet_context_menu").offset({top: RealY, left: RealX});
}

/*------Messages to C++------*/

function SendSimpleCommand( command )
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = command;

	SendWXMessage( JSON.stringify(tSend) );
}

function SendCommandWithPath( command, strPath )
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = command;
	tSend['data'] = {};
	tSend['data']['path'] = strPath;

	SendWXMessage( JSON.stringify(tSend) );
}

function SendMsg_GetRecentFile()  { SendSimpleCommand("get_recent_projects"); }
function OnClickNewProject()      { SendSimpleCommand("homepage_newproject"); }
function OnClickOpenProject()     { SendSimpleCommand("homepage_openproject"); }

function OnOpenRecentFile( strPath )
{
	SendCommandWithPath("homepage_open_recentfile", decodeURI(strPath));
}

function OnDeleteRecentFile()
{
	$("#recnet_context_menu").hide();

	/*----drop it from the list first, so the page does not wait for a round trip----*/
	let AllFile = $(".FileItem");
	let nFile = AllFile.length;
	for (let p = 0; p < nFile; p++)
	{
		if (AllFile[p].getAttribute("fpath") == RightBtnFilePath)
			$(AllFile[p]).remove();
	}

	UpdateRecentClearBtnDisplay();

	SendCommandWithPath("homepage_delete_recentfile", RightBtnFilePath);
}

function OnDeleteAllRecentFiles()
{
	$('#FileList').html('');
	UpdateRecentClearBtnDisplay();

	SendSimpleCommand("homepage_delete_all_recentfile");
}

function OnExploreRecentFile()
{
	SendCommandWithPath("homepage_explore_recentfile", decodeURI(RightBtnFilePath));

	$("#recnet_context_menu").hide();
}

function OutputKey(keyCode, isCtrlDown, isShiftDown, isCmdDown)
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "get_web_shortcut";
	tSend['key_event'] = {};
	tSend['key_event']['key']   = keyCode;
	tSend['key_event']['ctrl']  = isCtrlDown;
	tSend['key_event']['shift'] = isShiftDown;
	tSend['key_event']['cmd']   = isCmdDown;

	SendWXMessage(JSON.stringify(tSend));
}

/*---------------Global-----------------*/
window.postMessage = HandleStudio;

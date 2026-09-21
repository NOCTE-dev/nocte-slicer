function OnInit()
{
	TranslatePage();
	
	RequestProfile();
}

function HandleStudio(pVal)
{
	let strCmd=pVal['command'];
	//alert(strCmd);
	
	if(strCmd=='response_userguide_profile')
	{
		// NOCTE-BEGIN nocte-offline
		// ADR-003: filament selection is the last step; there is no stealth-mode page (../4orca)
		// and no network-plugin page (../5), so InstallNetworkPlugin() is gone and "Finish" is
		// always the primary button.
		m_ProfileItem=pVal['response'];
		SortUI();
		// NOCTE-END
	}
}

function ReturnPreviewPage()
{
	let nMode=m_ProfileItem["model"].length;
	
	if( nMode==1)
		document.location.href="../1/index.html";
	else
		document.location.href="../21/index.html";	
}

function FinishGuide()
{
	let bRet=ResponseFilamentResult();
	
	if(bRet)	
	{
		var tSend={};
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="user_guide_finish";
		tSend['data']={};
		tSend['data']['action']="finish";
		
		SendWXMessage( JSON.stringify(tSend) );	
	}
	//window.location.href="../6/index.html";
}
